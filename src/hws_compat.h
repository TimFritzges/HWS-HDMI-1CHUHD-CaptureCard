/* hws_compat.h */

#include <linux/version.h>
#include <media/v4l2-fh.h>

/*
 * Linux 7.0 removed vb2_ops.wait_prepare/wait_finish. Older kernels still
 * expose the callbacks and vb2 helpers, so keep using them where available.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0)
#define HWS_VB2_WAIT_OPS \
	.wait_prepare = vb2_ops_wait_prepare, \
	.wait_finish = vb2_ops_wait_finish,
#else
#define HWS_VB2_WAIT_OPS
#endif

/* v4l2 fh compat */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,18,0)
#define HWS_V4L2_FH_ADD(fh, file) v4l2_fh_add((fh), (file))
#define HWS_V4L2_FH_DEL(fh, file) v4l2_fh_del((fh), (file))
#else
#define HWS_V4L2_FH_ADD(fh, file) v4l2_fh_add((fh))
#define HWS_V4L2_FH_DEL(fh, file) v4l2_fh_del((fh))
#endif
