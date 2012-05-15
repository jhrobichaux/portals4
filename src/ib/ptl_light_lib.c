/**
 * @file ptl_ppe_client.c
 *
 * @brief Client side for PPE support.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "ptl_loc.h"

/*
 * per process global state
 * acquire proc_gbl_mutex before making changes
 * that require atomicity
 */
static pthread_mutex_t per_proc_gbl_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct {
	/* Communication with PPE. */
	size_t comm_pad_size;

	struct {
		/* Variable numbers of sbufs. */
		unsigned char buffers[0];
	} *comm_pad;

	struct ppe_comm_pad *ppe_comm_pad;

	/* Cookie given by the PPE to that client and used for almost
	 * any communication. */
	void *cookie;

	/* Count PtlInit/PtlFini */
	int	ref_cnt;
	int finalized;

	/* Virtual address of the slab containing the ppebufs on the
	 * PPE. */
	void *ppebufs_ppeaddr;

	/* Virtual address of the slab containing the ppebufs in this
	 * process. */
	void *ppebufs_addr;
} ppe;

void gbl_release(ref_t *ref)
{
	gbl_t *gbl = container_of(ref, gbl_t, ref);

	pthread_mutex_destroy(&gbl->gbl_mutex);
}

int gbl_init(gbl_t *gbl)
{
	pthread_mutex_init(&gbl->gbl_mutex, NULL);

	return PTL_OK;
}

/**
 * @brief Cleanup shared memory resources.
 *
 * @param[in] ni
 */
static void release_ppe_resources(void)
{
	if (ppe.ppe_comm_pad != MAP_FAILED) {
		munmap(ppe.ppe_comm_pad, sizeof(struct ppe_comm_pad));
		ppe.ppe_comm_pad = MAP_FAILED;
	}

	if (ppe.comm_pad) {
		free(ppe.comm_pad);
		ppe.comm_pad = NULL;
	}
}

static inline obj_t *dequeue_free_obj(pool_t *pool)
{
	union counted_ptr oldv, newv, retv;

	retv.c16 = pool->free_list.c16;

	do {
		oldv = retv;
		if (retv.obj != NULL) {
			obj_t *obj = ((void *)retv.obj) + (ppe.ppebufs_addr - ppe.ppebufs_ppeaddr);
			newv.obj = obj->next;
		} else {
			newv.obj = NULL;
		}
		newv.counter = oldv.counter + 1;

		retv.c16 = PtlInternalAtomicCas128(&pool->free_list.c16, oldv, newv);
	} while (retv.c16 != oldv.c16);

	return ((void *)retv.obj) + (ppe.ppebufs_addr - ppe.ppebufs_ppeaddr);
}

/**
 * Allocate a ppebuf from the shared memory pool.
 *
 * @param buf_p pointer to return value
 *
 * @return status
 */
static inline int ppebuf_alloc(ppebuf_t **buf_p)
{
	obj_t *obj;

#ifndef NO_ARG_VALIDATION
	if (!ppe.ppe_comm_pad)
		return PTL_NO_INIT;
#endif

	while ((obj = dequeue_free_obj(&ppe.ppe_comm_pad->ppebuf_pool)) == NULL) {
		SPINLOCK_BODY();
	}

	*buf_p = container_of(obj, ppebuf_t, obj);
	return PTL_OK;
}

static inline void enqueue_free_obj(pool_t *pool, obj_t *obj)
{
	union counted_ptr oldv, newv, tmpv;
	obj_t *ppe_obj = ((void *)obj) - ppe.ppebufs_addr + ppe.ppebufs_ppeaddr; /* vaddr of obj in PPE */

	tmpv.c16 = pool->free_list.c16;

	do {
		oldv = tmpv;
		obj->next = tmpv.obj;
		newv.obj = ppe_obj;
		newv.counter = oldv.counter + 1;
		tmpv.c16 = PtlInternalAtomicCas128(&pool->free_list.c16, oldv, newv);
	} while (tmpv.c16 != oldv.c16);
}

/**
 * Drop a reference to a ppebuf
 *
 * If the last reference has been dropped the buf
 * will be freed.
 *
 * @param buf on which to drop a reference
 *
 * @return status
 */
static inline void ppebuf_release(ppebuf_t *buf)
{
	obj_t *obj = (obj_t *)buf;

	__sync_synchronize();

	enqueue_free_obj(&ppe.ppe_comm_pad->ppebuf_pool, obj);
}

/* Format of the shared space (through XPMEM):
 * 0: PPE queue
 * 4KiB: rank 0 queue + buffers
 * ....
 */
static int setup_ppe(void)
{
	int try_count;
	int shm_fd = -1;
	int cmd_ret;

	ppe.ppe_comm_pad = MAP_FAILED;

	/*
	 * Allocate the buffers in memory to communicate with the PPE.
	 */

	/* Connect to the PPE shared memory. */
	/* Try for 10 seconds. That should leave enough time for the PPE
	 * to start and create the file. */
	try_count = 100;
	do {
		shm_fd = shm_open(COMM_PAD_FNAME, O_RDWR, S_IRUSR | S_IWUSR);

		if (shm_fd != -1)
			break;

		usleep(100000);		/* 100ms */
		try_count --;
	} while(try_count);

	if (shm_fd == -1) {
		ptl_warn("Couldn't open the shared memory file %s\n", COMM_PAD_FNAME);
		goto exit_fail;
	}

	/* Wait for the file to have the right size before mmaping it. */
	try_count = 100;
	do {
		struct stat buf;

		if (fstat(shm_fd, &buf) == -1) {
			ptl_warn("Couldn't fstat the shared memory file\n");
			goto exit_fail;
		}

		if (buf.st_size >= sizeof(struct ppe_comm_pad))
			break;

		usleep(100000);		/* 100ms */
		try_count --;
	} while(try_count);

	if (try_count >= 100000) {
		ptl_warn("Shared memory file has wrong size\n");
		goto exit_fail;
	}

	/* Map it. */
	ppe.ppe_comm_pad = mmap(NULL, sizeof(struct ppe_comm_pad),
								PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (ppe.ppe_comm_pad == MAP_FAILED) {
		ptl_warn("mmap failed (%d)", errno);
		perror("");
		goto exit_fail;
	}

	/* The share memory is mmaped, so we can close the file. */
	close(shm_fd);
	shm_fd = -1;

	/* Say hello to the PPE. */
	/* Step 0 -> 1. Reserve the PPE command field. Once it is 1, then
	 * the cmd field is ours, and no other client can claim until we
	 * are done. */
	switch_cmd_level(ppe.ppe_comm_pad, 0, 1);

	/* Fill the command. */
	ppe.ppe_comm_pad->cmd.pid = getpid();

	switch_cmd_level(ppe.ppe_comm_pad, 1, 2);

	/* Once done processing, the PPE will switch the level to
	 * 3. */
	while(ppe.ppe_comm_pad->cmd.level != 3)
		SPINLOCK_BODY();

	/* Process the reply. */
	ppe.cookie = ppe.ppe_comm_pad->cmd.cookie;
	ppe.ppebufs_ppeaddr = ppe.ppe_comm_pad->cmd.ppebufs_ppeaddr;
	ppe.ppebufs_addr = map_segment(&ppe.ppe_comm_pad->cmd.ppebufs_mapping);

	cmd_ret = (ppe.ppebufs_addr == NULL);

	/*	Switch it back to 0 for the other clients. */
	switch_cmd_level(ppe.ppe_comm_pad, 3, 0);

	if (cmd_ret) {
		WARN();
		goto exit_fail;
	}

	/* This client can now communicate through regular messages with the PPE. */
	return PTL_OK;

 exit_fail:
	if (shm_fd != -1)
		close(shm_fd);

	release_ppe_resources();

	return PTL_FAIL;
}

/* Transfer a message to the PPE and busy wait for the reply. */
static void transfer_msg(ppebuf_t *buf)
{
	/* Enqueue on the PPE queue. Since we know the virtual address on
	 * the PPE, that is what we give, and why we pass NULL as 1st
	 * parameter. */
	buf->obj.next = NULL;
	buf->completed = 0;
	buf->cookie = ppe.cookie;

	enqueue((void *)(uintptr_t)(ppe.ppebufs_addr - ppe.ppebufs_ppeaddr), &ppe.ppe_comm_pad->queue, (obj_t *)buf);

	/* Wait for the reply from the PPE. */
	while(buf->completed == 0)
		SPINLOCK_BODY();
}

int PtlInit(void)
{
	int ret;
	ppebuf_t *buf;

	ret = pthread_mutex_lock(&per_proc_gbl_mutex);
	if (ret) {
		ptl_warn("unable to acquire proc_gbl mutex\n");
		ret = PTL_FAIL;
		goto err0;
	}

	if (ppe.finalized) {
		ptl_warn("PtlInit after PtlFini\n");
		ret = PTL_FAIL;
		goto err1;
	}

	/* if first call to PtlInit do real initialization */
	if (ppe.ref_cnt == 0) {
		if (misc_init_once() != PTL_OK) {
			goto err1;
		}

		ret = setup_ppe();
		if (ret != PTL_OK) {
			goto err1;
		}
	}

	/* Call PPE now. */
	if ((ret = ppebuf_alloc(&buf))) {
		WARN();
		goto err1;
	}

	buf->op = OP_PtlInit;

	transfer_msg(buf);

	ret = buf->msg.ret;

	ppebuf_release(buf);

	if (ret)
		goto err0;

	ppe.ref_cnt++;

	pthread_mutex_unlock(&per_proc_gbl_mutex);

	return PTL_OK;

 err1:
	pthread_mutex_unlock(&per_proc_gbl_mutex);
 err0:
	return ret;
}

void PtlFini(void)
{
	int ret;

	ret = pthread_mutex_lock(&per_proc_gbl_mutex);
	if (ret) {
		ptl_warn("unable to acquire proc_gbl mutex\n");
		abort();
		goto err0;
	}

	/* this would be a bug */
	if (ppe.ref_cnt == 0) {
		ptl_warn("ref_cnt already 0 ?!!\n");
		goto err1;
	}

	ppe.ref_cnt--;

	if (ppe.ref_cnt == 0) {
		ppe.finalized = 1;

		//todo: cleanup.
	}

	pthread_mutex_unlock(&per_proc_gbl_mutex);

	return;

err1:
	pthread_mutex_unlock(&per_proc_gbl_mutex);
err0:
	return;
}

/* Passthrough operations. */

int PtlNIInit(ptl_interface_t        iface,
              unsigned int           options,
              ptl_pid_t              pid,
              const ptl_ni_limits_t *desired,
              ptl_ni_limits_t       *actual,
              ptl_handle_ni_t       *ni_handle)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlNIInit;

	buf->msg.PtlNIInit.iface = iface;
	buf->msg.PtlNIInit.options = options;
	buf->msg.PtlNIInit.pid = pid;
	if (desired) {
		buf->msg.PtlNIInit.with_desired = 1;
		buf->msg.PtlNIInit.desired = *desired;
	} else {
		buf->msg.PtlNIInit.with_desired = 0;
	}
	transfer_msg(buf);

	err = buf->msg.ret;

	if (actual)
		*actual = buf->msg.PtlNIInit.actual;

	*ni_handle = buf->msg.PtlNIInit.ni_handle;

	ppebuf_release(buf);

	return err;
}

int PtlNIFini(ptl_handle_ni_t ni_handle)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlNIFini;

	buf->msg.PtlNIFini.ni_handle = ni_handle;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlNIStatus(ptl_handle_ni_t ni_handle,
                ptl_sr_index_t  status_register,
                ptl_sr_value_t *status)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}


	buf->op = OP_PtlNIStatus;

	buf->msg.PtlNIStatus.ni_handle = ni_handle;
	buf->msg.PtlNIStatus.status_register = status_register;

	transfer_msg(buf);

	err = buf->msg.ret;

	*status = buf->msg.PtlNIStatus.status;

	ppebuf_release(buf);

	return err;
}

int PtlNIHandle(ptl_handle_any_t handle,
                ptl_handle_ni_t *ni_handle)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlNIHandle;

	buf->msg.PtlNIHandle.handle = handle;

	transfer_msg(buf);

	err = buf->msg.ret;

	*ni_handle = buf->msg.PtlNIHandle.ni_handle;

	ppebuf_release(buf);

	return err;
}

int PtlSetMap(ptl_handle_ni_t      ni_handle,
              ptl_size_t           map_size,
              const ptl_process_t *mapping)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlSetMap;

	buf->msg.PtlSetMap.ni_handle = ni_handle;
	buf->msg.PtlSetMap.map_size = map_size;

	err = create_mapping(mapping, map_size, &buf->msg.PtlSetMap.mapping);
	if (err)
		goto done;

	transfer_msg(buf);

	delete_mapping(&buf->msg.PtlSetMap.mapping);

	err = buf->msg.ret;

 done:
	ppebuf_release(buf);

	return err;
}

int PtlGetMap(ptl_handle_ni_t ni_handle,
              ptl_size_t      map_size,
              ptl_process_t  *mapping,
              ptl_size_t     *actual_map_size)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlGetMap;

	buf->msg.PtlGetMap.ni_handle = ni_handle;
	buf->msg.PtlGetMap.map_size = map_size;

	err = create_mapping(mapping, map_size, &buf->msg.PtlGetMap.mapping);
	if (err)
		goto done;

	transfer_msg(buf);

	delete_mapping(&buf->msg.PtlGetMap.mapping);

	*actual_map_size = buf->msg.PtlGetMap.actual_map_size;

	err = buf->msg.ret;

 done:
	ppebuf_release(buf);

	return err;
}

int PtlPTAlloc(ptl_handle_ni_t ni_handle,
               unsigned int    options,
               ptl_handle_eq_t eq_handle,
               ptl_pt_index_t  pt_index_req,
               ptl_pt_index_t *pt_index)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlPTAlloc;

	buf->msg.PtlPTAlloc.ni_handle = ni_handle;
	buf->msg.PtlPTAlloc.options = options;
	buf->msg.PtlPTAlloc.eq_handle = eq_handle;
	buf->msg.PtlPTAlloc.pt_index_req = pt_index_req;

	transfer_msg(buf);

	err = buf->msg.ret;

	*pt_index = buf->msg.PtlPTAlloc.pt_index;

	ppebuf_release(buf);

	return err;
}

int PtlPTFree(ptl_handle_ni_t ni_handle,
              ptl_pt_index_t  pt_index)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlPTFree;

	buf->msg.PtlPTFree.ni_handle = ni_handle;
	buf->msg.PtlPTFree.pt_index = pt_index;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlPTDisable(ptl_handle_ni_t ni_handle,
                 ptl_pt_index_t  pt_index)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlPTDisable;

	buf->msg.PtlPTDisable.ni_handle = ni_handle;
	buf->msg.PtlPTDisable.pt_index = pt_index;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlPTEnable(ptl_handle_ni_t ni_handle,
                ptl_pt_index_t  pt_index)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlPTEnable;

	buf->msg.PtlPTEnable.ni_handle = ni_handle;
	buf->msg.PtlPTEnable.pt_index = pt_index;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlGetUid(ptl_handle_ni_t ni_handle,
              ptl_uid_t      *uid)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlGetUid;

	buf->msg.PtlGetUid.ni_handle = ni_handle;

	transfer_msg(buf);

	err = buf->msg.ret;

	*uid = buf->msg.PtlGetUid.uid;

	ppebuf_release(buf);

	return err;
}

int PtlGetId(ptl_handle_ni_t ni_handle,
             ptl_process_t  *id)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlGetId;

	buf->msg.PtlGetId.ni_handle = ni_handle;

	transfer_msg(buf);

	err = buf->msg.ret;

	*id = buf->msg.PtlGetId.id;

	ppebuf_release(buf);

	return err;
}

int PtlGetPhysId(ptl_handle_ni_t ni_handle,
                 ptl_process_t  *id)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlGetPhysId;

	buf->msg.PtlGetPhysId.ni_handle = ni_handle;

	transfer_msg(buf);

	err = buf->msg.ret;

	*id = buf->msg.PtlGetPhysId.id;

	ppebuf_release(buf);

	return err;
}

static struct xpmem_map *create_iovec_mapping(const ptl_iovec_t *iov_list, int num_iov)
{
	struct xpmem_map *mapping;
	int i;
	int err;

	mapping = calloc(num_iov, sizeof(struct xpmem_map));
	if (!mapping) {
		WARN();
		return NULL;
	}

	for (i = 0; i < num_iov; i++) {
		const ptl_iovec_t *iov = &iov_list[i];
		
		err = create_mapping(iov->iov_base, iov->iov_len, &mapping[i]);
		if (err)
			goto err;
	}

	return mapping;

 err:
	for (i = 0; i < num_iov; i++) {
		delete_mapping(&mapping[i]);
	}

	free(mapping);

	return NULL;
}

static void destroy_iovec_mapping(struct xpmem_map *mapping, int num_iov)
{
	int i;

	for (i = 0; i < num_iov; i++) {
		delete_mapping(&mapping[i]);
	}

	free(mapping);
}

int PtlMDBind(ptl_handle_ni_t  ni_handle,
              const ptl_md_t  *md,
              ptl_handle_md_t *md_handle)
{
	ppebuf_t *buf;
	int err;
	struct xpmem_map *iovec_mapping = NULL;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlMDBind;

	buf->msg.PtlMDBind.ni_handle = ni_handle;
	buf->msg.PtlMDBind.md = *md;

	if (md->options & PTL_IOVEC) {
		iovec_mapping = create_iovec_mapping(md->start, md->length);
		if (!iovec_mapping) {
			err = PTL_NO_SPACE;
			goto done;
		}
		err = create_mapping(iovec_mapping, md->length*sizeof(struct xpmem_map),
							 &buf->msg.PtlMDBind.mapping);
		if (err) {
			destroy_iovec_mapping(iovec_mapping, md->length);
			goto done;
		}
	} else {
		err = create_mapping(md->start, md->length, &buf->msg.PtlMDBind.mapping);
		if (err)
			goto done;
	}

	transfer_msg(buf);

	err = buf->msg.ret;
	if (err) {
		delete_mapping(&buf->msg.PtlMDBind.mapping);
		if (md->options & PTL_IOVEC)
			destroy_iovec_mapping(iovec_mapping, md->length);
	} else {
		*md_handle = buf->msg.PtlMDBind.md_handle;
	}

 done:
	ppebuf_release(buf);

	return err;
}

int PtlMDRelease(ptl_handle_md_t md_handle)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlMDRelease;

	buf->msg.PtlMDRelease.md_handle = md_handle;

	transfer_msg(buf);

	err = buf->msg.ret;
	if (err == PTL_OK)
		delete_mapping(&buf->msg.PtlMDRelease.md_start);

	ppebuf_release(buf);

	return err;
}

int PtlLEAppend(ptl_handle_ni_t  ni_handle,
                ptl_pt_index_t   pt_index,
                const ptl_le_t  *le,
                ptl_list_t       ptl_list,
                void            *user_ptr,
                ptl_handle_le_t *le_handle)
{
	ppebuf_t *buf;
	int err;
	struct xpmem_map *iovec_mapping = NULL;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlLEAppend;

	buf->msg.PtlLEAppend.ni_handle = ni_handle;
	buf->msg.PtlLEAppend.pt_index = pt_index;
	buf->msg.PtlLEAppend.le = *le;
	buf->msg.PtlLEAppend.ptl_list = ptl_list;
	buf->msg.PtlLEAppend.user_ptr = user_ptr;

	if (le->options & PTL_IOVEC) {
		iovec_mapping = create_iovec_mapping(le->start, le->length);
		if (!iovec_mapping) {
			err = PTL_NO_SPACE;
			goto done;
		}
		err = create_mapping(iovec_mapping, le->length*sizeof(struct xpmem_map),
							 &buf->msg.PtlLEAppend.mapping);
		if (err) {
			destroy_iovec_mapping(iovec_mapping, le->length);
			goto done;
		}
	} else {
		err = create_mapping(le->start, le->length, &buf->msg.PtlLEAppend.mapping);
		if (err)
			goto done;
	}

	transfer_msg(buf);

	err = buf->msg.ret;
	if (err) {
		delete_mapping(&buf->msg.PtlLEAppend.mapping);
		if (le->options & PTL_IOVEC)
			destroy_iovec_mapping(iovec_mapping, le->length);
	} else {
		*le_handle = buf->msg.PtlLEAppend.le_handle;
	}
	
 done:
	ppebuf_release(buf);

	return err;
}

int PtlLEUnlink(ptl_handle_le_t le_handle)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlLEUnlink;

	buf->msg.PtlLEUnlink.le_handle = le_handle;

	transfer_msg(buf);

	err = buf->msg.ret;
	if (err == PTL_OK)
		delete_mapping(&buf->msg.PtlLEUnlink.le_start);

	ppebuf_release(buf);

	return err;
}

int PtlLESearch(ptl_handle_ni_t ni_handle,
                ptl_pt_index_t  pt_index,
                const ptl_le_t *le,
                ptl_search_op_t ptl_search_op,
                void           *user_ptr)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlLESearch;

	buf->msg.PtlLESearch.ni_handle = ni_handle;
	buf->msg.PtlLESearch.pt_index = pt_index;
	buf->msg.PtlLESearch.le = *le;
	buf->msg.PtlLESearch.ptl_search_op = ptl_search_op;
	buf->msg.PtlLESearch.user_ptr = user_ptr;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlMEAppend(ptl_handle_ni_t  ni_handle,
                ptl_pt_index_t   pt_index,
                const ptl_me_t  *me,
                ptl_list_t       ptl_list,
                void            *user_ptr,
                ptl_handle_me_t *me_handle)
{
	ppebuf_t *buf;
	int err;
	struct xpmem_map *iovec_mapping = NULL;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlMEAppend;

	buf->msg.PtlMEAppend.ni_handle = ni_handle;
	buf->msg.PtlMEAppend.pt_index = pt_index;
	buf->msg.PtlMEAppend.me = *me;
	buf->msg.PtlMEAppend.ptl_list = ptl_list;
	buf->msg.PtlMEAppend.user_ptr = user_ptr;

	if (me->options & PTL_IOVEC) {
		iovec_mapping = create_iovec_mapping(me->start, me->length);
		if (!iovec_mapping) {
			err = PTL_NO_SPACE;
			goto done;
		}
		err = create_mapping(iovec_mapping, me->length*sizeof(struct xpmem_map),
							 &buf->msg.PtlMEAppend.mapping);
		if (err) {
			destroy_iovec_mapping(iovec_mapping, me->length);
			goto done;
		}
	} else {
		err = create_mapping(me->start, me->length, &buf->msg.PtlMEAppend.mapping);
		if (err)
			goto done;
	}

	transfer_msg(buf);

	err = buf->msg.ret;
	if (err) {
		delete_mapping(&buf->msg.PtlMEAppend.mapping);
		if (me->options & PTL_IOVEC)
			destroy_iovec_mapping(iovec_mapping, me->length);
	} else {
		*me_handle = buf->msg.PtlMEAppend.me_handle;
	}
	
 done:
	ppebuf_release(buf);

	return err;
}

int PtlMEUnlink(ptl_handle_me_t me_handle)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlMEUnlink;

	buf->msg.PtlMEUnlink.me_handle = me_handle;

	transfer_msg(buf);

	err = buf->msg.ret;
	if (err == PTL_OK)
		delete_mapping(&buf->msg.PtlMEUnlink.me_start);

	ppebuf_release(buf);

	return err;
}

int PtlMESearch(ptl_handle_ni_t ni_handle,
                ptl_pt_index_t  pt_index,
                const ptl_me_t *me,
                ptl_search_op_t ptl_search_op,
                void           *user_ptr)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlMESearch;

	buf->msg.PtlMESearch.ni_handle = ni_handle;
	buf->msg.PtlMESearch.pt_index = pt_index;
	buf->msg.PtlMESearch.me = *me;
	buf->msg.PtlMESearch.ptl_search_op = ptl_search_op;
	buf->msg.PtlMESearch.user_ptr = user_ptr;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

//todo: protect with lock
struct light_ct {
	struct list_head list;
	ptl_handle_ct_t ct_handle;
	struct xpmem_map ct_mapping;
	struct ct_info *info;
};
PTL_LIST_HEAD(CTs_list);

static struct light_ct *get_light_ct(ptl_handle_eq_t ct_handle)
{
	struct light_ct *ct;
	struct list_head *l;

	list_for_each(l, &CTs_list) {
		ct = list_entry(l, struct light_ct, list);
		if (ct->ct_handle == ct_handle)
			return ct;
	}

	return NULL;
}

int PtlCTAlloc(ptl_handle_ni_t  ni_handle,
               ptl_handle_ct_t *ct_handle)
{
	ppebuf_t *buf;
	int err;
	struct light_ct *ct;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	ct = calloc(1, sizeof(struct light_ct));
	if (!ct) {
		err = PTL_NO_SPACE;
		goto done;
	}

	buf->op = OP_PtlCTAlloc;

	buf->msg.PtlCTAlloc.ni_handle = ni_handle;

	transfer_msg(buf);

	err = buf->msg.ret;

	if (err == PTL_OK) {
		ct->ct_handle = buf->msg.PtlCTAlloc.ct_handle;
		ct->ct_mapping = buf->msg.PtlCTAlloc.ct_mapping;

		ct->info = map_segment(&ct->ct_mapping);
		if (!ct->info) {
			// call ctfree
			abort();
		}

		*ct_handle = buf->msg.PtlCTAlloc.ct_handle;

		/* Store the new CT locally. */
		list_add(&ct->list, &CTs_list);
	}

 done:
	ppebuf_release(buf);

	return err;
}

int PtlCTFree(ptl_handle_ct_t ct_handle)
{
	ppebuf_t *buf;
	int err;
	struct light_ct *ct = get_light_ct(ct_handle);

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlCTFree;

	buf->msg.PtlCTFree.ct_handle = ct_handle;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	if (err == PTL_OK) {
		/* It's unclear whether unmapping after the segment has been
		 * destroyed on the PPE is an error. */
		unmap_segment(&ct->ct_mapping);
		list_del(&ct->list);
		free(ct);
	}

	return err;
}

int PtlCTCancelTriggered(ptl_handle_ct_t ct_handle)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlCTCancelTriggered;

	buf->msg.PtlCTCancelTriggered.ct_handle = ct_handle;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlCTGet(ptl_handle_ct_t ct_handle,
             ptl_ct_event_t *event)
{
	const struct light_ct *ct;
	int err;

#ifndef NO_ARG_VALIDATION
	if (!ppe.ppe_comm_pad)
		return PTL_NO_INIT;
#endif

	ct = get_light_ct(ct_handle);
	if (ct) {
		*event = ct->info->event;
		err = PTL_OK;
	} else {
		err = PTL_ARG_INVALID;
	}

	return err;
}

int PtlCTWait(ptl_handle_ct_t ct_handle,
              ptl_size_t      test,
              ptl_ct_event_t *event)
{
	const struct light_ct *ct;
	int err;

#ifndef NO_ARG_VALIDATION
	if (!ppe.ppe_comm_pad)
		return PTL_NO_INIT;
#endif

	ct = get_light_ct(ct_handle);
	if (ct)
		err = PtlCTWait_work(ct->info, test, event);
	else
		err = PTL_ARG_INVALID;

	return err;
}

int PtlCTPoll(const ptl_handle_ct_t *ct_handles,
              const ptl_size_t      *tests,
              unsigned int           size,
              ptl_time_t             timeout,
              ptl_ct_event_t        *event,
              unsigned int          *which)
{
	int err;
	int i;
	struct ct_info **cts_info = NULL;

#ifndef NO_ARG_VALIDATION
	if (!ppe.ppe_comm_pad)
		return PTL_NO_INIT;
#endif

	if (size == 0) {
		err = PTL_ARG_INVALID;
		goto done;
	}

	cts_info = malloc(size * sizeof(struct ct_info));
	if (!cts_info) {
		err = PTL_NO_SPACE;
		goto done;
	}

	for (i = 0; i < size; i++) {
		struct light_ct *ct = get_light_ct(ct_handles[i]);
		if (!ct) {
			err = PTL_ARG_INVALID;
			goto done;
		}
		cts_info[i] = ct->info;
	}

	err = PtlCTPoll_work(cts_info, tests, size, timeout, event, which);

 done:
	if (cts_info)
		free(cts_info);

	return err;
}

int PtlCTSet(ptl_handle_ct_t ct_handle,
             ptl_ct_event_t  new_ct)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlCTSet;

	buf->msg.PtlCTSet.ct_handle = ct_handle;
	buf->msg.PtlCTSet.new_ct = new_ct;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}


int PtlCTInc(ptl_handle_ct_t ct_handle,
             ptl_ct_event_t  increment)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlCTInc;

	buf->msg.PtlCTInc.ct_handle = ct_handle;
	buf->msg.PtlCTInc.increment = increment;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlPut(ptl_handle_md_t  md_handle,
           ptl_size_t       local_offset,
           ptl_size_t       length,
           ptl_ack_req_t    ack_req,
           ptl_process_t    target_id,
           ptl_pt_index_t   pt_index,
           ptl_match_bits_t match_bits,
           ptl_size_t       remote_offset,
           void            *user_ptr,
           ptl_hdr_data_t   hdr_data)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlPut;

	buf->msg.PtlPut.md_handle = md_handle;
	buf->msg.PtlPut.local_offset = local_offset;
	buf->msg.PtlPut.length = length;
	buf->msg.PtlPut.ack_req = ack_req;
	buf->msg.PtlPut.target_id = target_id;
	buf->msg.PtlPut.pt_index = pt_index;
	buf->msg.PtlPut.match_bits = match_bits;
	buf->msg.PtlPut.remote_offset = remote_offset;
	buf->msg.PtlPut.user_ptr = user_ptr;
	buf->msg.PtlPut.hdr_data = hdr_data;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlGet(ptl_handle_md_t  md_handle,
           ptl_size_t       local_offset,
           ptl_size_t       length,
           ptl_process_t    target_id,
           ptl_pt_index_t   pt_index,
           ptl_match_bits_t match_bits,
           ptl_size_t       remote_offset,
           void            *user_ptr)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlGet;

	buf->msg.PtlGet.md_handle = md_handle;
	buf->msg.PtlGet.local_offset = local_offset;
	buf->msg.PtlGet.length = length;
	buf->msg.PtlGet.target_id = target_id;
	buf->msg.PtlGet.pt_index = pt_index;
	buf->msg.PtlGet.match_bits = match_bits;
	buf->msg.PtlGet.remote_offset = remote_offset;
	buf->msg.PtlGet.user_ptr = user_ptr;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlAtomic(ptl_handle_md_t  md_handle,
              ptl_size_t       local_offset,
              ptl_size_t       length,
              ptl_ack_req_t    ack_req,
              ptl_process_t    target_id,
              ptl_pt_index_t   pt_index,
              ptl_match_bits_t match_bits,
              ptl_size_t       remote_offset,
              void            *user_ptr,
              ptl_hdr_data_t   hdr_data,
              ptl_op_t         operation,
              ptl_datatype_t   datatype)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlAtomic;

	buf->msg.PtlAtomic.md_handle = md_handle;
	buf->msg.PtlAtomic.local_offset = local_offset;
	buf->msg.PtlAtomic.length = length;
	buf->msg.PtlAtomic.ack_req = ack_req;
	buf->msg.PtlAtomic.target_id = target_id;
	buf->msg.PtlAtomic.pt_index = pt_index;
	buf->msg.PtlAtomic.match_bits = match_bits;
	buf->msg.PtlAtomic.remote_offset = remote_offset;
	buf->msg.PtlAtomic.user_ptr = user_ptr;
	buf->msg.PtlAtomic.hdr_data = hdr_data;
	buf->msg.PtlAtomic.operation = operation;
	buf->msg.PtlAtomic.datatype = datatype;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlFetchAtomic(ptl_handle_md_t  get_md_handle,
                   ptl_size_t       local_get_offset,
                   ptl_handle_md_t  put_md_handle,
                   ptl_size_t       local_put_offset,
                   ptl_size_t       length,
                   ptl_process_t    target_id,
                   ptl_pt_index_t   pt_index,
                   ptl_match_bits_t match_bits,
                   ptl_size_t       remote_offset,
                   void            *user_ptr,
                   ptl_hdr_data_t   hdr_data,
                   ptl_op_t         operation,
                   ptl_datatype_t   datatype)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlFetchAtomic;

	buf->msg.PtlFetchAtomic.get_md_handle = get_md_handle;
	buf->msg.PtlFetchAtomic.local_get_offset = local_get_offset;
	buf->msg.PtlFetchAtomic.put_md_handle = put_md_handle;
	buf->msg.PtlFetchAtomic.local_put_offset = local_put_offset;
	buf->msg.PtlFetchAtomic.length = length;
	buf->msg.PtlFetchAtomic.target_id = target_id;
	buf->msg.PtlFetchAtomic.pt_index = pt_index;
	buf->msg.PtlFetchAtomic.match_bits = match_bits;
	buf->msg.PtlFetchAtomic.remote_offset = remote_offset;
	buf->msg.PtlFetchAtomic.user_ptr = user_ptr;
	buf->msg.PtlFetchAtomic.hdr_data = hdr_data;
	buf->msg.PtlFetchAtomic.operation = operation;
	buf->msg.PtlFetchAtomic.datatype = datatype;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlSwap(ptl_handle_md_t  get_md_handle,
            ptl_size_t       local_get_offset,
            ptl_handle_md_t  put_md_handle,
            ptl_size_t       local_put_offset,
            ptl_size_t       length,
            ptl_process_t    target_id,
            ptl_pt_index_t   pt_index,
            ptl_match_bits_t match_bits,
            ptl_size_t       remote_offset,
            void            *user_ptr,
            ptl_hdr_data_t   hdr_data,
            const void      *operand,
            ptl_op_t         operation,
            ptl_datatype_t   datatype)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlSwap;

	buf->msg.PtlSwap.get_md_handle = get_md_handle;
	buf->msg.PtlSwap.local_get_offset = local_get_offset;
	buf->msg.PtlSwap.put_md_handle = put_md_handle;
	buf->msg.PtlSwap.local_put_offset = local_put_offset;
	buf->msg.PtlSwap.length = length;
	buf->msg.PtlSwap.target_id = target_id;
	buf->msg.PtlSwap.pt_index = pt_index;
	buf->msg.PtlSwap.match_bits = match_bits;
	buf->msg.PtlSwap.remote_offset = remote_offset;
	buf->msg.PtlSwap.user_ptr = user_ptr;
	buf->msg.PtlSwap.hdr_data = hdr_data;
	buf->msg.PtlSwap.operand = operand;
	buf->msg.PtlSwap.operation = operation;
	buf->msg.PtlSwap.datatype = datatype;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlAtomicSync(void)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlAtomicSync;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

//todo: protect with lock
struct light_eq {
	struct list_head list;
	ptl_handle_eq_t eq_handle;
	struct xpmem_map eqe_list_map;
	struct eqe_list *eqe_list;
};
PTL_LIST_HEAD(EQs_list);

static struct light_eq *get_light_eq(ptl_handle_eq_t eq_handle)
{
	struct light_eq *eq;
	struct list_head *l;

	list_for_each(l, &EQs_list) {
		eq = list_entry(l, struct light_eq, list);
		if (eq->eq_handle == eq_handle)
			return eq;
	}

	return NULL;
}

int PtlEQAlloc(ptl_handle_ni_t  ni_handle,
               ptl_size_t       count,
               ptl_handle_eq_t *eq_handle)
{
	ppebuf_t *buf;
	int err;
	struct light_eq *eq;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	eq = calloc(1, sizeof(struct light_eq));
	if (!eq) {
		err = PTL_NO_SPACE;
		goto done;
	}

	buf->op = OP_PtlEQAlloc;

	buf->msg.PtlEQAlloc.ni_handle = ni_handle;
	buf->msg.PtlEQAlloc.count = count;

	transfer_msg(buf);

	err = buf->msg.ret;

	*eq_handle = buf->msg.PtlEQAlloc.eq_handle;

	if (err == PTL_OK) {
		struct eqe_list *local_eqe_list;

		eq->eqe_list_map = buf->msg.PtlEQAlloc.eqe_list;
		local_eqe_list = map_segment(&eq->eqe_list_map);

		if (local_eqe_list == NULL) {
			// todo: call eqfree and return an error
			abort();
		}

		/* Store the new EQ locally. */
		eq->eq_handle = *eq_handle;
		eq->eqe_list = local_eqe_list;
		list_add(&eq->list, &EQs_list);
	}

 done:
	ppebuf_release(buf);

	return err;
}

int PtlEQFree(ptl_handle_eq_t eq_handle)
{
	ppebuf_t *buf;
	int err;
	struct light_eq *eq;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	eq = get_light_eq(eq_handle);
	if (!eq) {
		err = PTL_ARG_INVALID;
		goto done;
	}

	buf->op = OP_PtlEQFree;

	buf->msg.PtlEQFree.eq_handle = eq_handle;

	transfer_msg(buf);

	err = buf->msg.ret;

	if (err == PTL_OK) {
		/* It's unclear whether unmapping after the segment has been
		 * destroyed on the PPE is an error. */
		unmap_segment(&eq->eqe_list_map);
		list_del(&eq->list);
		free(eq);
	}

 done:
	ppebuf_release(buf);

	return err;
}

int PtlEQGet(ptl_handle_eq_t eq_handle,
             ptl_event_t    *event)
{
	const struct light_eq *eq;
	int err;

#ifndef NO_ARG_VALIDATION
	if (!ppe.ppe_comm_pad)
		return PTL_NO_INIT;
#endif

	eq = get_light_eq(eq_handle);
	if (eq) {
		err = PtlEQGet_work(eq->eqe_list, event);
	} else {
		err = PTL_ARG_INVALID;
	}

	return err;
}

int PtlEQWait(ptl_handle_eq_t eq_handle,
              ptl_event_t    *event)
{
	const struct light_eq *eq;
	int err;

#ifndef NO_ARG_VALIDATION
	if (!ppe.ppe_comm_pad)
		return PTL_NO_INIT;
#endif

	eq = get_light_eq(eq_handle);
	if (eq)
		err = PtlEQWait_work(eq->eqe_list, event);
	else
		err = PTL_ARG_INVALID;

	return err;
}

int PtlEQPoll(const ptl_handle_eq_t *eq_handles,
              unsigned int           size,
              ptl_time_t             timeout,
              ptl_event_t           *event,
              unsigned int          *which)
{
	int err;
	int i;
	struct eqe_list **eqes_list = NULL;

#ifndef NO_ARG_VALIDATION
	if (!ppe.ppe_comm_pad)
		return PTL_NO_INIT;
#endif

	if (size == 0) {
		err = PTL_ARG_INVALID;
		goto done;
	}

	eqes_list = malloc(size*sizeof(struct eqe_list));
	if (!eqes_list) {
		err = PTL_NO_SPACE;
		goto done;
	}

	for (i = 0; i < size; i++) {
		struct light_eq *eq = get_light_eq(eq_handles[i]);
		if (!eq) {
			err = PTL_ARG_INVALID;
			goto done;
		}
		eqes_list[i] = eq->eqe_list;
	}

	err = PtlEQPoll_work(eqes_list, size,
						 timeout, event, which);

 done:
	if (eqes_list)
		free(eqes_list);

	return err;
}

int PtlTriggeredPut(ptl_handle_md_t  md_handle,
                    ptl_size_t       local_offset,
                    ptl_size_t       length,
                    ptl_ack_req_t    ack_req,
                    ptl_process_t    target_id,
                    ptl_pt_index_t   pt_index,
                    ptl_match_bits_t match_bits,
                    ptl_size_t       remote_offset,
                    void            *user_ptr,
                    ptl_hdr_data_t   hdr_data,
                    ptl_handle_ct_t  trig_ct_handle,
                    ptl_size_t       threshold)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlTriggeredPut;

	buf->msg.PtlTriggeredPut.md_handle = md_handle;
	buf->msg.PtlTriggeredPut.local_offset = local_offset;
	buf->msg.PtlTriggeredPut.length = length;
	buf->msg.PtlTriggeredPut.ack_req = ack_req;
	buf->msg.PtlTriggeredPut.target_id = target_id;
	buf->msg.PtlTriggeredPut.pt_index = pt_index;
	buf->msg.PtlTriggeredPut.match_bits = match_bits;
	buf->msg.PtlTriggeredPut.remote_offset = remote_offset;
	buf->msg.PtlTriggeredPut.user_ptr = user_ptr;
	buf->msg.PtlTriggeredPut.hdr_data = hdr_data;
	buf->msg.PtlTriggeredPut.trig_ct_handle = trig_ct_handle;
	buf->msg.PtlTriggeredPut.threshold = threshold;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlTriggeredGet(ptl_handle_md_t  md_handle,
                    ptl_size_t       local_offset,
                    ptl_size_t       length,
                    ptl_process_t    target_id,
                    ptl_pt_index_t   pt_index,
                    ptl_match_bits_t match_bits,
                    ptl_size_t       remote_offset,
                    void            *user_ptr,
                    ptl_handle_ct_t  trig_ct_handle,
                    ptl_size_t       threshold)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlTriggeredGet;

	buf->msg.PtlTriggeredGet.md_handle = md_handle;
	buf->msg.PtlTriggeredGet.local_offset = local_offset;
	buf->msg.PtlTriggeredGet.length = length;
	buf->msg.PtlTriggeredGet.target_id = target_id;
	buf->msg.PtlTriggeredGet.pt_index = pt_index;
	buf->msg.PtlTriggeredGet.match_bits = match_bits;
	buf->msg.PtlTriggeredGet.remote_offset = remote_offset;
	buf->msg.PtlTriggeredGet.user_ptr = user_ptr;
	buf->msg.PtlTriggeredGet.trig_ct_handle = trig_ct_handle;
	buf->msg.PtlTriggeredGet.threshold = threshold;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlTriggeredAtomic(ptl_handle_md_t  md_handle,
                       ptl_size_t       local_offset,
                       ptl_size_t       length,
                       ptl_ack_req_t    ack_req,
                       ptl_process_t    target_id,
                       ptl_pt_index_t   pt_index,
                       ptl_match_bits_t match_bits,
                       ptl_size_t       remote_offset,
                       void            *user_ptr,
                       ptl_hdr_data_t   hdr_data,
                       ptl_op_t         operation,
                       ptl_datatype_t   datatype,
                       ptl_handle_ct_t  trig_ct_handle,
                       ptl_size_t       threshold)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlTriggeredAtomic;

	buf->msg.PtlTriggeredAtomic.md_handle = md_handle;
	buf->msg.PtlTriggeredAtomic.local_offset = local_offset;
	buf->msg.PtlTriggeredAtomic.length = length;
	buf->msg.PtlTriggeredAtomic.ack_req = ack_req;
	buf->msg.PtlTriggeredAtomic.target_id = target_id;
	buf->msg.PtlTriggeredAtomic.pt_index = pt_index;
	buf->msg.PtlTriggeredAtomic.match_bits = match_bits;
	buf->msg.PtlTriggeredAtomic.remote_offset = remote_offset;
	buf->msg.PtlTriggeredAtomic.user_ptr = user_ptr;
	buf->msg.PtlTriggeredAtomic.hdr_data = hdr_data;
	buf->msg.PtlTriggeredAtomic.operation = operation;
	buf->msg.PtlTriggeredAtomic.datatype = datatype;
	buf->msg.PtlTriggeredAtomic.trig_ct_handle = trig_ct_handle;
	buf->msg.PtlTriggeredAtomic.threshold = threshold;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlTriggeredFetchAtomic(ptl_handle_md_t  get_md_handle,
                            ptl_size_t       local_get_offset,
                            ptl_handle_md_t  put_md_handle,
                            ptl_size_t       local_put_offset,
                            ptl_size_t       length,
                            ptl_process_t    target_id,
                            ptl_pt_index_t   pt_index,
                            ptl_match_bits_t match_bits,
                            ptl_size_t       remote_offset,
                            void            *user_ptr,
                            ptl_hdr_data_t   hdr_data,
                            ptl_op_t         operation,
                            ptl_datatype_t   datatype,
                            ptl_handle_ct_t  trig_ct_handle,
                            ptl_size_t       threshold)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlTriggeredFetchAtomic;

	buf->msg.PtlTriggeredFetchAtomic.get_md_handle = get_md_handle;
	buf->msg.PtlTriggeredFetchAtomic.local_get_offset = local_get_offset;
	buf->msg.PtlTriggeredFetchAtomic.put_md_handle = put_md_handle;
	buf->msg.PtlTriggeredFetchAtomic.local_put_offset = local_put_offset;
	buf->msg.PtlTriggeredFetchAtomic.length = length;
	buf->msg.PtlTriggeredFetchAtomic.target_id = target_id;
	buf->msg.PtlTriggeredFetchAtomic.pt_index = pt_index;
	buf->msg.PtlTriggeredFetchAtomic.match_bits = match_bits;
	buf->msg.PtlTriggeredFetchAtomic.remote_offset = remote_offset;
	buf->msg.PtlTriggeredFetchAtomic.user_ptr = user_ptr;
	buf->msg.PtlTriggeredFetchAtomic.hdr_data = hdr_data;
	buf->msg.PtlTriggeredFetchAtomic.operation = operation;
	buf->msg.PtlTriggeredFetchAtomic.datatype = datatype;
	buf->msg.PtlTriggeredFetchAtomic.trig_ct_handle = trig_ct_handle;
	buf->msg.PtlTriggeredFetchAtomic.threshold = threshold;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlTriggeredSwap(ptl_handle_md_t  get_md_handle,
                     ptl_size_t       local_get_offset,
                     ptl_handle_md_t  put_md_handle,
                     ptl_size_t       local_put_offset,
                     ptl_size_t       length,
                     ptl_process_t    target_id,
                     ptl_pt_index_t   pt_index,
                     ptl_match_bits_t match_bits,
                     ptl_size_t       remote_offset,
                     void            *user_ptr,
                     ptl_hdr_data_t   hdr_data,
                     const void      *operand,
                     ptl_op_t         operation,
                     ptl_datatype_t   datatype,
                     ptl_handle_ct_t  trig_ct_handle,
                     ptl_size_t       threshold)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlTriggeredSwap;

	buf->msg.PtlTriggeredSwap.get_md_handle = get_md_handle;
	buf->msg.PtlTriggeredSwap.local_get_offset = local_get_offset;
	buf->msg.PtlTriggeredSwap.put_md_handle = put_md_handle;
	buf->msg.PtlTriggeredSwap.local_put_offset = local_put_offset;
	buf->msg.PtlTriggeredSwap.length = length;
	buf->msg.PtlTriggeredSwap.target_id = target_id;
	buf->msg.PtlTriggeredSwap.pt_index = pt_index;
	buf->msg.PtlTriggeredSwap.match_bits = match_bits;
	buf->msg.PtlTriggeredSwap.remote_offset = remote_offset;
	buf->msg.PtlTriggeredSwap.user_ptr = user_ptr;
	buf->msg.PtlTriggeredSwap.hdr_data = hdr_data;
	buf->msg.PtlTriggeredSwap.operand = operand;
	buf->msg.PtlTriggeredSwap.operation = operation;
	buf->msg.PtlTriggeredSwap.datatype = datatype;
	buf->msg.PtlTriggeredSwap.trig_ct_handle = trig_ct_handle;
	buf->msg.PtlTriggeredSwap.threshold = threshold;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlTriggeredCTInc(ptl_handle_ct_t ct_handle,
                      ptl_ct_event_t  increment,
                      ptl_handle_ct_t trig_ct_handle,
                      ptl_size_t      threshold)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlTriggeredCTInc;

	buf->msg.PtlTriggeredCTInc.ct_handle = ct_handle;
	buf->msg.PtlTriggeredCTInc.increment = increment;
	buf->msg.PtlTriggeredCTInc.trig_ct_handle = trig_ct_handle;
	buf->msg.PtlTriggeredCTInc.threshold = threshold;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlTriggeredCTSet(ptl_handle_ct_t ct_handle,
                      ptl_ct_event_t  new_ct,
                      ptl_handle_ct_t trig_ct_handle,
                      ptl_size_t      threshold)
{
	ppebuf_t *buf;
	int err;

	if ((err = ppebuf_alloc(&buf))) {
		WARN();
		return err;
	}

	buf->op = OP_PtlTriggeredCTSet;

	buf->msg.PtlTriggeredCTSet.ct_handle = ct_handle;
	buf->msg.PtlTriggeredCTSet.new_ct = new_ct;
	buf->msg.PtlTriggeredCTSet.trig_ct_handle = trig_ct_handle;
	buf->msg.PtlTriggeredCTSet.threshold = threshold;

	transfer_msg(buf);

	err = buf->msg.ret;

	ppebuf_release(buf);

	return err;
}

int PtlStartBundle(ptl_handle_ni_t ni_handle)
{
	return PTL_OK;
}

int PtlEndBundle(ptl_handle_ni_t ni_handle)
{
	return PTL_OK;
}
