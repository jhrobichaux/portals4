/**
 * @file ptl_rdma.c
 *
 * @brief RDMA operations used by target.
 */
#include "ptl_loc.h"

/**
 * @brief Build and post an RDMA read/write work request to transfer
 * data to/from one or more local memory segments from/to a single remote
 * memory segment.
 *
 * @param[in] buf A buf holding state for the rdma operation.
 * @param[in] qp The InfiniBand QP to send the rdma to.
 * @param[in] dir The rdma direction in or out.
 * @param[in] raddr The remote address at the initiator.
 * @param[in] rkey The rkey of the InfiniBand MR that registers the
 * memory region at the initiator that includes the data segment.
 * @param[in] sg_list The scatter/gather list that contains
 * the local addresses, lengths and lkeys.
 * @param[in] num_sge The size of the scatter/gather array.
 * @param[in] comp A flag indicating whether to generate a completion
 * event when this operation is complete.
 *
 * @return status
 */
static int post_rdma(buf_t *buf, struct ibv_qp *qp, data_dir_t dir,
		     uint64_t raddr, uint32_t rkey,
		     struct ibv_sge *sg_list, int num_sge, uint8_t comp)
{
	int err;
	struct ibv_send_wr wr;
	struct ibv_send_wr *bad_wr;

	/* build an infiniband rdma write work request */
	if (likely(comp)) {
		wr.wr_id = (uintptr_t)buf;
		wr.send_flags = IBV_SEND_SIGNALED;
	} else {
		if (atomic_inc(&buf->conn->rdma.completion_threshold) == get_param(PTL_MAX_SEND_COMP_THRESHOLD)) {
			buf->rdma.send_wr.send_flags = IBV_SEND_SIGNALED;
			atomic_set(&buf->conn->rdma.completion_threshold, 0);
		} else {
			wr.wr_id = 0;
			wr.send_flags = 0;
		}
	}

	wr.next	= NULL;
	wr.sg_list = sg_list;
	wr.num_sge = num_sge;
	wr.opcode = (dir == DATA_DIR_IN) ? IBV_WR_RDMA_READ :
					   IBV_WR_RDMA_WRITE;
	wr.wr.rdma.remote_addr = raddr;
	wr.wr.rdma.rkey	= rkey;
#ifdef USE_XRC
	wr.xrc_remote_srq_num = buf->dest.xrc_remote_srq_num;
#endif

	/* post the work request to the QP send queue for the
	 * destination/initiator */
	err = ibv_post_send(buf->dest.rdma.qp, &wr, &bad_wr);
	if (err)
		return PTL_FAIL;

	return PTL_OK;
}

/**
 * @brief Build the local scatter gather list for a target RDMA operation.
 *
 * The most general case is transfering from an iovec to an iovec.
 * This requires a double loop iterating over the memory segments
 * at the (remote) initiator and also over the memory segments in the
 * (local) target list element. This routine implements the loop over
 * the local memory segments building an InfiniBand scatter/gather
 * array to be used in an rdma operation. It is called by process_rdma
 * below which implements the outer loop over the remote memory segments.
 * The case where one or both the MD and the LE/ME do not have an iovec
 * are handled as limits of the general case.
 *
 * @param[in] buf The message buffer received by the target.
 * @param[in,out] cur_index_p The current index in the LE/ME iovec.
 * @param[in,out] cur_off_p The offset into the current LE/ME
 * iovec element.
 * @param[in] sge The scatter/gather array to fill in.
 * @param[in] sge_size The size of the scatter/gather array.
 * @param[out] num_sge_p The number of sge entries used.
 * @param[out] mr_list A list of MRs used to map the local memory segments.
 * @param[in,out] length_p On input the requested number of bytes to be
 * transfered in the current rdma operation. On exit the actual number
 * of bytes transfered.
 *
 * @return status
 */
static int build_sge(buf_t *buf,
		     ptl_size_t *cur_index_p,
		     ptl_size_t *cur_off_p,
		     struct ibv_sge *sge,
		     int sge_size,
		     int *num_sge_p,
		     mr_t **mr_list,
		     ptl_size_t *length_p)
{
	int err;
	ni_t *ni = obj_to_ni(buf);
	me_t *me = buf->me;
	mr_t *mr;
	ptl_iovec_t *iov = iov;
	ptl_size_t bytes;
	ptl_size_t cur_index = *cur_index_p;
	ptl_size_t cur_off = *cur_off_p;
	ptl_size_t cur_len = 0;
	int num_sge = 0;
	void * addr;
	ptl_size_t resid = *length_p;

	while (resid) {
		/* compute the starting address and
		 * length of the next sge entry */
		bytes = resid;

		if (unlikely(me->num_iov)) {
			iov = ((ptl_iovec_t *)me->start) + cur_index;
			addr = iov->iov_base + cur_off;

			if (bytes > iov->iov_len - cur_off)
				bytes = iov->iov_len - cur_off;
		} else {
			addr = me->start + cur_off;
			assert(bytes <= me->length - cur_off);
		}

		/* lookup the mr for the current local segment */
		err = mr_lookup(ni, addr, bytes, &mr);
		if (err)
			return err;

		sge->addr = (uintptr_t)addr;
		sge->length = bytes;
		sge->lkey = mr->ibmr->lkey;

		/* save the mr and the reference to it until
		 * we receive a completion */
		*mr_list++ = mr;

		/* update the dma info */
		resid -= bytes;
		cur_len += bytes;
		cur_off += bytes;

		if (unlikely(me->num_iov)) {
			if (cur_off >= iov->iov_len) {
				cur_index++;
				cur_off = 0;
			}
		}

		if (bytes) {
			sge++;
			if (++num_sge >= sge_size)
				break;
		}
	}

	*num_sge_p = num_sge;
	*cur_index_p = cur_index;
	*cur_off_p = cur_off;
	*length_p = cur_len;

	return PTL_OK;
}

/**
 * @brief Allocate a temporary buf to hold mr references for an rdma operation.
 *
 * @param[in] buf The message buf.
 *
 * @return the buf
 */
static buf_t *tgt_alloc_rdma_buf(buf_t *buf)
{
	buf_t *rdma_buf;
	int err;

	err = buf_alloc(obj_to_ni(buf), &rdma_buf);
	if (err) {
		WARN();
		return NULL;
	}

	rdma_buf->type = BUF_RDMA;
	rdma_buf->xxbuf = buf;
	buf_get(buf);
	rdma_buf->dest = buf->dest;
	rdma_buf->conn = buf->conn;

	return rdma_buf;
}

/**
 * @brief Issue one or more InfiniBand RDMA from target to initiator
 * based on target transfer state.
 *
 * This routine is called from the tgt state machine for InfiniBand
 * transfers if there is data to transfer between initiator and
 * target that cannot be sent as immediate data.
 *
 * Each time this routine is called it issues as many rdma operations as
 * possible up to a limit or finishes the operation. The
 * current state of the rdma transfer(s) is contained in the buf->rdma
 * struct. Each rdma operation transfers data between one or more local
 * memory segments in an LE/ME and a single contiguous remote segment.
 * The number of local segments is limited by the size of the remote
 * segment and the maximum number of scatter/gather array elements.
 *
 * @param[in] buf The message buffer received by the target.
 *
 * @return status
 */
static int process_rdma(buf_t *buf)
{
	int err;
	uint64_t addr;
	ptl_size_t bytes;
	ptl_size_t iov_index;
	ptl_size_t iov_off;
	data_dir_t dir;
	ptl_size_t resid;
	int comp = 0;
	buf_t *rdma_buf;
	int sge_size = get_param(PTL_MAX_QP_SEND_SGE);
	struct ibv_sge sge_list[sge_size];
	int entries = 0;
	int cur_rdma_ops = 0;
	size_t rem_off;
	uint32_t rem_size;
	struct ibv_sge *rem_sge;
	uint32_t rem_key;
	int max_rdma_ops = get_param(PTL_MAX_RDMA_WR_OUT);
	mr_t **mr_list;

	dir = buf->rdma_dir;
	resid = (dir == DATA_DIR_IN) ? buf->put_resid : buf->get_resid;
	iov_index = buf->cur_loc_iov_index;
	iov_off = buf->cur_loc_iov_off;

	rem_sge = buf->rdma.cur_rem_sge;
	rem_off = buf->rdma.cur_rem_off;
	rem_size = le32_to_cpu(rem_sge->length);
	rem_key = le32_to_cpu(rem_sge->lkey);

	/* try to generate additional rdma operations as long
	 * as there is remaining data to transfer and we have
	 * not exceeded the maximum number of outstanding rdma
	 * operations that we allow ourselves. rdma_comp is
	 * incremented when we have reached this limit and
	 * will get cleared when we receive get send completions
	 * from the CQ. We do not reenter the state machine
	 * until we have received a send completion so
	 * rdma_comp should have been cleared */

	assert(!atomic_read(&buf->rdma.rdma_comp));

	while (resid) {
		/* compute remote starting address and
		 * and length of the next rdma transfer */
		addr = le64_to_cpu(rem_sge->addr) + rem_off;

		bytes = resid;
		if (bytes > rem_size - rem_off)
			bytes = rem_size - rem_off;

		rdma_buf = tgt_alloc_rdma_buf(buf);
		if (!rdma_buf)
			return PTL_FAIL;

		mr_list = rdma_buf->mr_list + rdma_buf->num_mr;

		/* build a local scatter/gather array on our stack
		 * to transfer as many bytes as possible from the
		 * LE/ME up to rlength. The transfer size may be
		 * limited by the size of the scatter/gather list
		 * sge_list. The new values of iov_index and iov_offset
		 * are returned as well as the number of bytes
		 * transferred. */
		err = build_sge(buf, &iov_index, &iov_off, sge_list,
				sge_size, &entries, mr_list, &bytes);
		if (err) {
			buf_put(rdma_buf);
			return err;
		}

		rdma_buf->num_mr += entries;

		/* add the rdma_buf to a list of pending rdma
		 * transfers at the buf. These will get
		 * cleaned up in tgt_cleanup. The mr's will
		 * get dropped in buf_cleanup */
		pthread_spin_lock(&buf->rdma_list_lock);
		list_add_tail(&rdma_buf->list, &buf->rdma_list);
		pthread_spin_unlock(&buf->rdma_list_lock);

		/* update dma info */
		resid -= bytes;
		rem_off += bytes;

		if (resid && rem_off >= rem_size) {
			rem_sge++;
			rem_size = le32_to_cpu(rem_sge->length);
			rem_key = le32_to_cpu(rem_sge->lkey);
			rem_off = 0;
		}

		/* if we are finished or have reached the limit
		 * of the number of rdma's outstanding then
		 * request a completion notification */
		if (!resid || ++cur_rdma_ops >= max_rdma_ops) {
			comp = 1;
 			atomic_inc(&buf->rdma.rdma_comp);
 		}

		rdma_buf->comp = comp;

		/* post the rdma read or write operation to the QP */
		err = post_rdma(rdma_buf, buf->dest.rdma.qp, dir,
				addr, rem_key, sge_list, entries, comp);
		if (err) {
			pthread_spin_lock(&buf->rdma_list_lock);
			list_del(&rdma_buf->list);
			pthread_spin_unlock(&buf->rdma_list_lock);
			return err;
		}

		if (comp)
			break;
	}

	/* update the current rdma state */
	buf->cur_loc_iov_index = iov_index;
	buf->cur_loc_iov_off = iov_off;
	buf->rdma.cur_rem_off = rem_off;
	buf->rdma.cur_rem_sge = rem_sge;

	if (dir == DATA_DIR_IN)
		buf->put_resid = resid;
	else
		buf->get_resid = resid;

	return PTL_OK;
}

struct transport transport_rdma = {
	.type = CONN_TYPE_RDMA,
	.post_tgt_dma = process_rdma,
	.send_message = send_message_rdma,
};

/**
 * @brief Request the indirect scatter/gather list.
 *
 * @param[in] buf The message buffer received by the target.
 *
 * @return status
 */
int process_rdma_desc(buf_t *buf)
{
	int err;
	ni_t *ni = obj_to_ni(buf);
	data_t *data;
	uint64_t raddr;
	uint32_t rkey;
	uint32_t rlen;
	struct ibv_sge sge;
	int num_sge;
	int comp;
	void *indir_sge;
	mr_t *mr;

	data = buf->rdma_dir == DATA_DIR_IN ? buf->data_in : buf->data_out;

	raddr = le64_to_cpu(data->rdma.sge_list[0].addr);
	rkey = le32_to_cpu(data->rdma.sge_list[0].lkey);
	rlen = le32_to_cpu(data->rdma.sge_list[0].length);

	indir_sge = malloc(rlen);
	if (!indir_sge) {
		WARN();
		err = PTL_FAIL;
		goto err1;
	}

	if (mr_lookup(ni, indir_sge, rlen, &mr)) {
		WARN();
		err = PTL_FAIL;
		goto err1;
	}

	buf->indir_sge = indir_sge;
	buf->mr_list[buf->num_mr++] = mr;

	sge.addr = (uintptr_t)indir_sge;
	sge.lkey = mr->ibmr->lkey;
	sge.length = rlen;

	num_sge = 1;
	comp = 1;

	/* use the buf as its own rdma buf. */
	buf->comp = 1;
	buf->xxbuf = buf;
	buf->type = BUF_RDMA;

	err = post_rdma(buf, buf->dest.rdma.qp, DATA_DIR_IN,
			raddr, rkey, &sge, num_sge, comp);
	if (err) {
		err = PTL_FAIL;
		goto err1;
	}

	err = PTL_OK;
err1:
	return err;
}
