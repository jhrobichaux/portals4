int gbl_init(gbl_t *gbl);
extern int ptl_log_level;
extern unsigned long pagesize;
extern unsigned int linesize;

extern int ptl_env_polling_interval;
extern char *ptl_env_affinity;


#ifdef IS_PPE
int ppe_misc_init_once(void);
#else
int misc_init_once(void);
#endif
