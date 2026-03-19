/*
*/

#include <linux/pci.h>
#include <linux/kernel.h>
#include <media/videobuf2-core.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-vmalloc.h>
#include <media/videobuf2-dma-contig.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>
#include <sound/initval.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/ktime.h>
#include "hws.h"
#include "hws_reg.h"
#include "hws_compat.h"

static void hws_adapters_init(struct hws_pcie_dev *dev);
static void hws_get_video_param(struct hws_pcie_dev *dev,int index);
//static void StartDma(int index);
//static void StopDma(int index);
static int StartVideoCapture(struct hws_pcie_dev *pdx,int index);
static int StartAudioCapture(struct hws_pcie_dev *pdx,int index);
static void StopAudioCapture(struct hws_pcie_dev *pdx,int index);
static void StopVideoCapture(struct hws_pcie_dev *pdx,int index);
static int diag_enable;
module_param_named(diag_enable, diag_enable, int, 0644);
MODULE_PARM_DESC(diag_enable, "Enable HWS runtime diagnostics (0=off, 1=on)");

static int video_work_budget = 2;
module_param_named(video_work_budget, video_work_budget, int, 0644);
MODULE_PARM_DESC(video_work_budget, "Max queued buffers handled per video worker run (min 1)");

static int audio_work_budget = 8;
module_param_named(audio_work_budget, audio_work_budget, int, 0644);
MODULE_PARM_DESC(audio_work_budget, "Max queued audio packets handled per audio worker run (min 1)");

static int audio_period_bytes = 1024;
module_param_named(audio_period_bytes, audio_period_bytes, int, 0644);
MODULE_PARM_DESC(audio_period_bytes, "Requested ALSA period size in bytes (default 1024 = 256 frames @ 48kHz stereo s16)");

static int audio_periods = 8;
module_param_named(audio_periods, audio_periods, int, 0644);
MODULE_PARM_DESC(audio_periods, "Requested ALSA period count (default 8)");

static int audio_trace_enable;
module_param_named(audio_trace_enable, audio_trace_enable, int, 0644);
MODULE_PARM_DESC(audio_trace_enable, "Enable trace hooks for audio drop/silence events (0=off, 1=on)");

enum {
	HWS_FPS_POLICY_SOURCE_TRUTH = 1,
	HWS_FPS_POLICY_USERSPACE_CONVERT = 2,
	HWS_FPS_POLICY_DRIVER_CONVERT_EXPERIMENTAL = 3,
	HWS_FPS_POLICY_AUTO_PROFILE = 4,
};

static int fps_policy_mode = HWS_FPS_POLICY_AUTO_PROFILE;
module_param_named(fps_policy_mode, fps_policy_mode, int, 0644);
MODULE_PARM_DESC(fps_policy_mode, "FPS policy: 1=source-truth, 2=userspace-convert, 3=driver-convert (experimental), 4=auto-profile");

static int fps_policy_allow_experimental;
module_param_named(fps_policy_allow_experimental, fps_policy_allow_experimental, int, 0644);
MODULE_PARM_DESC(fps_policy_allow_experimental, "Allow experimental FPS policy mode 3 (0=off, 1=on)");

static inline bool hws_diag_enabled(void)
{
	return diag_enable != 0;
}

static inline bool hws_audio_trace_enabled(void)
{
	return audio_trace_enable != 0;
}

struct hws_audio_diag_stats {
	atomic64_t work_runs;
	atomic64_t buffers_found;
	atomic64_t delivered_bytes;
	atomic64_t delivered_frames;
	atomic64_t period_elapsed_calls;
	atomic64_t oversized_packets;
	atomic64_t dropped_no_substream;
	atomic64_t dropped_bad_runtime;
	atomic64_t dropped_ring_not_ready;
	atomic64_t no_video_silence_injects;
	atomic64_t fallback_silence_injects;
	atomic64_t timer_silence_injects;
	atomic64_t no_free_queue_slots;
	atomic64_t memcopy_failures;
	atomic64_t bad_packet_sizes;
	atomic64_t workqueue_requeues;
	atomic64_t stream_not_running;
	atomic64_t timer_runs;
	atomic64_t irq_to_copy_ns_total;
	atomic64_t irq_to_copy_ns_max;
	atomic64_t irq_to_copy_samples;
	atomic64_t copy_to_deliver_ns_total;
	atomic64_t copy_to_deliver_ns_max;
	atomic64_t copy_to_deliver_samples;
	atomic64_t irq_to_deliver_ns_total;
	atomic64_t irq_to_deliver_ns_max;
	atomic64_t irq_to_deliver_samples;
	atomic64_t queue_free_slots_min;
	atomic64_t queue_free_slots_max;
	atomic64_t queue_free_slots_total;
	atomic64_t queue_free_slots_samples;
	atomic64_t delivery_errors;
};

struct hws_diag_stats {
	atomic64_t work_runs;
	atomic64_t work_ns_total;
	atomic64_t work_ns_max;
	atomic64_t buf_processed;
	atomic64_t buf_done;
	atomic64_t buf_error;
	atomic64_t copy_path_frames;
	atomic64_t scaler_path_frames;
	atomic64_t novideo_frames;
	atomic64_t miss_frame_fallbacks;
	atomic64_t memcopy_ns_total;
	atomic64_t scaler_ns_total;
	atomic64_t fresh_frame_runs;
	atomic64_t nofresh_frame_runs;
	atomic64_t source_interval_ns_total;
	atomic64_t source_interval_ns_max;
	atomic64_t source_interval_samples;
};

static struct hws_diag_stats hws_diag[MAX_VID_CHANNELS];
static struct hws_audio_diag_stats hws_audio_diag[MAX_VID_CHANNELS];
static struct dentry *hws_diag_root;
static u64 hws_diag_last_fresh_ns[MAX_VID_CHANNELS];
static u64 hws_source_interval_ns_avg[MAX_VID_CHANNELS];

static inline void hws_diag_reset(void)
{
	int i;

	for (i = 0; i < MAX_VID_CHANNELS; i++) {
		atomic64_set(&hws_diag[i].work_runs, 0);
		atomic64_set(&hws_diag[i].work_ns_total, 0);
		atomic64_set(&hws_diag[i].work_ns_max, 0);
		atomic64_set(&hws_diag[i].buf_processed, 0);
		atomic64_set(&hws_diag[i].buf_done, 0);
		atomic64_set(&hws_diag[i].buf_error, 0);
		atomic64_set(&hws_diag[i].copy_path_frames, 0);
		atomic64_set(&hws_diag[i].scaler_path_frames, 0);
		atomic64_set(&hws_diag[i].novideo_frames, 0);
		atomic64_set(&hws_diag[i].miss_frame_fallbacks, 0);
		atomic64_set(&hws_diag[i].memcopy_ns_total, 0);
		atomic64_set(&hws_diag[i].scaler_ns_total, 0);
		atomic64_set(&hws_diag[i].fresh_frame_runs, 0);
		atomic64_set(&hws_diag[i].nofresh_frame_runs, 0);
		atomic64_set(&hws_diag[i].source_interval_ns_total, 0);
		atomic64_set(&hws_diag[i].source_interval_ns_max, 0);
		atomic64_set(&hws_diag[i].source_interval_samples, 0);
		hws_diag_last_fresh_ns[i] = 0;
		hws_source_interval_ns_avg[i] = 0;
		atomic64_set(&hws_audio_diag[i].work_runs, 0);
		atomic64_set(&hws_audio_diag[i].buffers_found, 0);
		atomic64_set(&hws_audio_diag[i].delivered_bytes, 0);
		atomic64_set(&hws_audio_diag[i].delivered_frames, 0);
		atomic64_set(&hws_audio_diag[i].period_elapsed_calls, 0);
		atomic64_set(&hws_audio_diag[i].oversized_packets, 0);
		atomic64_set(&hws_audio_diag[i].dropped_no_substream, 0);
		atomic64_set(&hws_audio_diag[i].dropped_bad_runtime, 0);
		atomic64_set(&hws_audio_diag[i].dropped_ring_not_ready, 0);
		atomic64_set(&hws_audio_diag[i].no_video_silence_injects, 0);
		atomic64_set(&hws_audio_diag[i].fallback_silence_injects, 0);
		atomic64_set(&hws_audio_diag[i].timer_silence_injects, 0);
		atomic64_set(&hws_audio_diag[i].no_free_queue_slots, 0);
		atomic64_set(&hws_audio_diag[i].memcopy_failures, 0);
		atomic64_set(&hws_audio_diag[i].bad_packet_sizes, 0);
		atomic64_set(&hws_audio_diag[i].workqueue_requeues, 0);
		atomic64_set(&hws_audio_diag[i].stream_not_running, 0);
		atomic64_set(&hws_audio_diag[i].timer_runs, 0);
		atomic64_set(&hws_audio_diag[i].irq_to_copy_ns_total, 0);
		atomic64_set(&hws_audio_diag[i].irq_to_copy_ns_max, 0);
		atomic64_set(&hws_audio_diag[i].irq_to_copy_samples, 0);
		atomic64_set(&hws_audio_diag[i].copy_to_deliver_ns_total, 0);
		atomic64_set(&hws_audio_diag[i].copy_to_deliver_ns_max, 0);
		atomic64_set(&hws_audio_diag[i].copy_to_deliver_samples, 0);
		atomic64_set(&hws_audio_diag[i].irq_to_deliver_ns_total, 0);
		atomic64_set(&hws_audio_diag[i].irq_to_deliver_ns_max, 0);
		atomic64_set(&hws_audio_diag[i].irq_to_deliver_samples, 0);
		atomic64_set(&hws_audio_diag[i].queue_free_slots_min, MAX_AUDIO_QUEUE);
		atomic64_set(&hws_audio_diag[i].queue_free_slots_max, 0);
		atomic64_set(&hws_audio_diag[i].queue_free_slots_total, 0);
		atomic64_set(&hws_audio_diag[i].queue_free_slots_samples, 0);
		atomic64_set(&hws_audio_diag[i].delivery_errors, 0);
	}
}

static inline void hws_diag_update_max(atomic64_t *slot, u64 value)
{
	u64 old;

	for (;;) {
		old = atomic64_read(slot);
		if (value <= old)
			return;
		if (atomic64_cmpxchg(slot, old, value) == old)
			return;
	}
}


static inline void hws_diag_update_min(atomic64_t *slot, u64 value)
{
	u64 old;

	for (;;) {
		old = atomic64_read(slot);
		if (value >= old)
			return;
		if (atomic64_cmpxchg(slot, old, value) == old)
			return;
	}
}

static inline void hws_audio_diag_record_latency(int ch, atomic64_t *total,
					 atomic64_t *max, atomic64_t *samples, u64 value_ns)
{
	if (ch < 0 || ch >= MAX_VID_CHANNELS)
		return;
	atomic64_add((s64)value_ns, total);
	hws_diag_update_max(max, value_ns);
	atomic64_inc(samples);
}

static inline void hws_audio_diag_record_queue_free(int ch, u32 free_slots)
{
	if (ch < 0 || ch >= MAX_VID_CHANNELS)
		return;
	hws_diag_update_min(&hws_audio_diag[ch].queue_free_slots_min, free_slots);
	hws_diag_update_max(&hws_audio_diag[ch].queue_free_slots_max, free_slots);
	atomic64_add((s64)free_slots, &hws_audio_diag[ch].queue_free_slots_total);
	atomic64_inc(&hws_audio_diag[ch].queue_free_slots_samples);
}

static inline u32 hws_audio_frame_bytes(const struct hws_audio *drv)
{
	if (!drv || drv->channels == 0)
		return 0;
	return 2U * (u32)drv->channels;
}

static u32 hws_audio_effective_packet_bytes(struct hws_audio *drv, u32 requested_bytes)
{
	u32 frame_bytes;
	u32 packet_bytes;

	if (!drv)
		return 0;

	frame_bytes = hws_audio_frame_bytes(drv);
	packet_bytes = requested_bytes;
	if (!packet_bytes && drv->dev)
		packet_bytes = READ_ONCE(drv->dev->m_dwAudioPTKSize);
	if (!packet_bytes && frame_bytes)
		packet_bytes = READ_ONCE(drv->period_size_byframes) * frame_bytes;
	if (!frame_bytes || !packet_bytes)
		return 0;

	packet_bytes -= packet_bytes % frame_bytes;
	return packet_bytes;
}

static u64 hws_audio_packet_duration_ns(struct hws_audio *drv, u32 packet_bytes)
{
	u32 frame_bytes;
	u32 sample_rate;
	u64 frames;

	if (!drv)
		return 0;

	frame_bytes = hws_audio_frame_bytes(drv);
	sample_rate = READ_ONCE(drv->sample_rate_out);
	packet_bytes = hws_audio_effective_packet_bytes(drv, packet_bytes);
	if (!frame_bytes || !sample_rate || !packet_bytes)
		return 0;

	frames = packet_bytes / frame_bytes;
	if (!frames)
		return 0;

	return div_u64(frames * NSEC_PER_SEC, sample_rate);
}

static unsigned long hws_audio_fallback_delay_jiffies(struct hws_audio *drv, u32 packet_bytes)
{
	u64 packet_ns;
	u64 delay_ns;
	u64 delay_us;
	unsigned long delay;

	packet_ns = hws_audio_packet_duration_ns(drv, packet_bytes);
	if (!packet_ns)
		return 1;

	delay_ns = max_t(u64, packet_ns / 2, NSEC_PER_MSEC);
	delay_us = div_u64(delay_ns + NSEC_PER_USEC - 1, NSEC_PER_USEC);
	delay = usecs_to_jiffies((unsigned int)delay_us);
	if (!delay)
		delay = 1;

	return delay;
}

static unsigned int hws_audio_init_period_constraints(struct hws_audio *drv,
						      unsigned int min_period_bytes)
{
	unsigned int packet_bytes;
	unsigned int frame_bytes;
	unsigned int bytes;
	unsigned int count = 0;

	if (!drv)
		return 0;

	packet_bytes = hws_audio_effective_packet_bytes(drv, 0);
	frame_bytes = hws_audio_frame_bytes(drv);
	if (!packet_bytes || !frame_bytes)
		return 0;

	if (min_period_bytes < 1024U)
		min_period_bytes = 1024U;
	min_period_bytes -= min_period_bytes % frame_bytes;
	if (!min_period_bytes)
		min_period_bytes = frame_bytes;

	for (bytes = min_period_bytes;
	     bytes <= packet_bytes && count < ARRAY_SIZE(drv->period_bytes_choices);
	     bytes <<= 1) {
		if ((packet_bytes % bytes) != 0)
			continue;
		drv->period_bytes_choices[count++] = bytes;
	}

	if (!count) {
		bytes = packet_bytes - (packet_bytes % frame_bytes);
		if (bytes) {
			drv->period_bytes_choices[0] = bytes;
			count = 1;
		}
	}

	drv->period_bytes_constraint.count = count;
	drv->period_bytes_constraint.list = drv->period_bytes_choices;
	drv->period_bytes_constraint.mask = 0;

	return count;
}

static bool hws_audio_stream_active(struct hws_audio *drv)
{
	struct hws_pcie_dev *pdx;
	int ch;

	if (!drv || !READ_ONCE(drv->substream))
		return false;

	pdx = drv->dev;
	ch = drv->index;
	if (!pdx || ch < 0 || ch >= MAX_VID_CHANNELS)
		return false;

	return READ_ONCE(pdx->m_bAudioRun[ch]) && READ_ONCE(pdx->m_bACapStarted[ch]);
}

enum hws_audio_silence_reason {
	HWS_AUDIO_SILENCE_NO_VIDEO = 0,
	HWS_AUDIO_SILENCE_FALLBACK,
	HWS_AUDIO_SILENCE_TIMER,
};

static int hws_diag_show(struct seq_file *m, void *unused)
{
	int i;

	seq_puts(m, "ch work_runs work_ns_total work_ns_max buf_processed buf_done buf_error copy_frames scaler_frames novideo_frames miss_fallbacks memcopy_ns_total scaler_ns_total fresh_runs nofresh_runs src_interval_ns_total src_interval_ns_max src_interval_samples\n");
	for (i = 0; i < MAX_VID_CHANNELS; i++) {
		seq_printf(m,
			"%d %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\n",
			i,
			(long long)atomic64_read(&hws_diag[i].work_runs),
			(long long)atomic64_read(&hws_diag[i].work_ns_total),
			(long long)atomic64_read(&hws_diag[i].work_ns_max),
			(long long)atomic64_read(&hws_diag[i].buf_processed),
			(long long)atomic64_read(&hws_diag[i].buf_done),
			(long long)atomic64_read(&hws_diag[i].buf_error),
			(long long)atomic64_read(&hws_diag[i].copy_path_frames),
			(long long)atomic64_read(&hws_diag[i].scaler_path_frames),
			(long long)atomic64_read(&hws_diag[i].novideo_frames),
			(long long)atomic64_read(&hws_diag[i].miss_frame_fallbacks),
			(long long)atomic64_read(&hws_diag[i].memcopy_ns_total),
			(long long)atomic64_read(&hws_diag[i].scaler_ns_total),
			(long long)atomic64_read(&hws_diag[i].fresh_frame_runs),
			(long long)atomic64_read(&hws_diag[i].nofresh_frame_runs),
			(long long)atomic64_read(&hws_diag[i].source_interval_ns_total),
			(long long)atomic64_read(&hws_diag[i].source_interval_ns_max),
			(long long)atomic64_read(&hws_diag[i].source_interval_samples));
	}

	return 0;
}

static int hws_diag_open(struct inode *inode, struct file *file)
{
	return single_open(file, hws_diag_show, inode->i_private);
}

static int hws_audio_diag_show(struct seq_file *m, void *unused)
{
	int i;

	seq_puts(m, "ch work_runs buffers_found delivered_bytes delivered_frames period_elapsed oversized_packets drop_no_substream drop_bad_runtime drop_ring_not_ready no_video_silence_injects fallback_silence_injects timer_silence_injects no_free_queue_slots memcopy_failures bad_packet_sizes workqueue_requeues stream_not_running timer_runs irq_to_copy_ns_total irq_to_copy_ns_max irq_to_copy_samples copy_to_deliver_ns_total copy_to_deliver_ns_max copy_to_deliver_samples irq_to_deliver_ns_total irq_to_deliver_ns_max irq_to_deliver_samples queue_free_slots_min queue_free_slots_max queue_free_slots_total queue_free_slots_samples delivery_errors\n");
	for (i = 0; i < MAX_VID_CHANNELS; i++) {
		seq_printf(m, "%d %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\n",
			i,
			(long long)atomic64_read(&hws_audio_diag[i].work_runs),
			(long long)atomic64_read(&hws_audio_diag[i].buffers_found),
			(long long)atomic64_read(&hws_audio_diag[i].delivered_bytes),
			(long long)atomic64_read(&hws_audio_diag[i].delivered_frames),
			(long long)atomic64_read(&hws_audio_diag[i].period_elapsed_calls),
			(long long)atomic64_read(&hws_audio_diag[i].oversized_packets),
			(long long)atomic64_read(&hws_audio_diag[i].dropped_no_substream),
			(long long)atomic64_read(&hws_audio_diag[i].dropped_bad_runtime),
			(long long)atomic64_read(&hws_audio_diag[i].dropped_ring_not_ready),
			(long long)atomic64_read(&hws_audio_diag[i].no_video_silence_injects),
			(long long)atomic64_read(&hws_audio_diag[i].fallback_silence_injects),
			(long long)atomic64_read(&hws_audio_diag[i].timer_silence_injects),
			(long long)atomic64_read(&hws_audio_diag[i].no_free_queue_slots),
			(long long)atomic64_read(&hws_audio_diag[i].memcopy_failures),
			(long long)atomic64_read(&hws_audio_diag[i].bad_packet_sizes),
			(long long)atomic64_read(&hws_audio_diag[i].workqueue_requeues),
			(long long)atomic64_read(&hws_audio_diag[i].stream_not_running),
			(long long)atomic64_read(&hws_audio_diag[i].timer_runs),
			(long long)atomic64_read(&hws_audio_diag[i].irq_to_copy_ns_total),
			(long long)atomic64_read(&hws_audio_diag[i].irq_to_copy_ns_max),
			(long long)atomic64_read(&hws_audio_diag[i].irq_to_copy_samples),
			(long long)atomic64_read(&hws_audio_diag[i].copy_to_deliver_ns_total),
			(long long)atomic64_read(&hws_audio_diag[i].copy_to_deliver_ns_max),
			(long long)atomic64_read(&hws_audio_diag[i].copy_to_deliver_samples),
			(long long)atomic64_read(&hws_audio_diag[i].irq_to_deliver_ns_total),
			(long long)atomic64_read(&hws_audio_diag[i].irq_to_deliver_ns_max),
			(long long)atomic64_read(&hws_audio_diag[i].irq_to_deliver_samples),
			(long long)atomic64_read(&hws_audio_diag[i].queue_free_slots_min),
			(long long)atomic64_read(&hws_audio_diag[i].queue_free_slots_max),
			(long long)atomic64_read(&hws_audio_diag[i].queue_free_slots_total),
			(long long)atomic64_read(&hws_audio_diag[i].queue_free_slots_samples),
			(long long)atomic64_read(&hws_audio_diag[i].delivery_errors));
	}

	return 0;
}

static int hws_audio_diag_open(struct inode *inode, struct file *file)
{
	return single_open(file, hws_audio_diag_show, inode->i_private);
}

static const struct file_operations hws_audio_diag_fops = {
	.owner = THIS_MODULE,
	.open = hws_audio_diag_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations hws_diag_fops = {
	.owner = THIS_MODULE,
	.open = hws_diag_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void hws_diag_init_debugfs(void)
{
	if (hws_diag_root)
		return;

	hws_diag_root = debugfs_create_dir("hwsuhdx1", NULL);
	if (IS_ERR_OR_NULL(hws_diag_root)) {
		hws_diag_root = NULL;
		return;
	}

	debugfs_create_file("video_diag", 0444, hws_diag_root, NULL, &hws_diag_fops);
	debugfs_create_file("audio_diag", 0444, hws_diag_root, NULL, &hws_audio_diag_fops);
}

static void hws_diag_remove_debugfs(void)
{
	if (!hws_diag_root)
		return;

	debugfs_remove_recursive(hws_diag_root);
	hws_diag_root = NULL;
}
static void InitVideoSys(struct hws_pcie_dev *pdx,int set);

//------------------------

static	void VideoScaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void FHD_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void HD_To_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void FHD_To_800X600_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void HD_To_800X600_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void SD_PAL_To_SD_NTSC_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void SD_NTSC_To_SD_PAL_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void SD_PAL_To_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void SD_PAL_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void SD_NTSC_To_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void SD_NTSC_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);	
static	void V1280X1024_To_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void V1280X1024_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void V1280X1024_To_800X600_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void FHD_To_SD_NTSC_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);	
static	void FHD_To_SD_PAL_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void HD_To_SD_NTSC_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);		
static	void HD_To_SD_PAL_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);		
static	void V1280X1024_NTSC_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void V1280X1024_PAL_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
static	void All_VideoScaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h);
//---------------------------
#define MAKE_ENTRY( __vend, __chip, __subven, __subdev, __configptr) {	\
	.vendor		= (__vend),					\
	.device		= (__chip),					\
	.subvendor	= (__subven),					\
	.subdevice	= (__subdev),					\
	.driver_data	= (unsigned long) (__configptr)			\
}
//MAKE_ENTRY(0x1F33, 0x9001, 0x1F33, 0x0008, NULL),
static const struct pci_device_id hws_pci_table[] = {
	MAKE_ENTRY(0x8888, 0x8581, 0x8888, 0x0007, NULL),
	MAKE_ENTRY(0x8888, 0x85A1, 0x8888, 0x0007, NULL),
	MAKE_ENTRY(0x1F33, 0x8581, 0x8888, 0x0007, NULL),
	MAKE_ENTRY(0x8888, 0x8591, 0x8888, 0x0007, NULL),
	{ }
};
//------------------
static const v4l2_model_timing_t support_videofmt[]= {
	[V4L2_MODEL_VIDEOFORMAT_1920X1080P60]	= V4L2_MODEL_TIMING(1920,1080,60,0),
	[V4L2_MODEL_VIDEOFORMAT_3840X2160P60]	 = V4L2_MODEL_TIMING(3840,2160,60,0),
	[V4L2_MODEL_VIDEOFORMAT_1280X720P60]	= V4L2_MODEL_TIMING(1280,720,60,0),
    [V4L2_MODEL_VIDEOFORMAT_720X480P60]		= V4L2_MODEL_TIMING(720,480,60,0),
    [V4L2_MODEL_VIDEOFORMAT_720X576P50]		= V4L2_MODEL_TIMING(720,480,50,0),
    [V4L2_MODEL_VIDEOFORMAT_800X600P60]		= V4L2_MODEL_TIMING(800,600,60,0),
    [V4L2_MODEL_VIDEOFORMAT_1024X768P60]	= V4L2_MODEL_TIMING(1024,768,60,0),
    [V4L2_MODEL_VIDEOFORMAT_1280X768P60]	= V4L2_MODEL_TIMING(1280,768,60,0),
    [V4L2_MODEL_VIDEOFORMAT_1280X800P60]	= V4L2_MODEL_TIMING(1280,800,60,0),
    [V4L2_MODEL_VIDEOFORMAT_1280X1024P60]	= V4L2_MODEL_TIMING(1280,1024,60,0),
    [V4L2_MODEL_VIDEOFORMAT_1360X768P60]	= V4L2_MODEL_TIMING(1360,768,60,0),
    [V4L2_MODEL_VIDEOFORMAT_1440X900P60]	= V4L2_MODEL_TIMING(1440,900,60,0),
    [V4L2_MODEL_VIDEOFORMAT_1680X1050P60]	= V4L2_MODEL_TIMING(1680,1050,60,0),
    [V4L2_MODEL_VIDEOFORMAT_1920X1200P60]	= V4L2_MODEL_TIMING(1920,1200,60,0),
    [V4L2_MODEL_VIDEOFORMAT_2560X1080P60]	= V4L2_MODEL_TIMING(2560,1080,60,0),
    [V4L2_MODEL_VIDEOFORMAT_2560X1440P60]	= V4L2_MODEL_TIMING(2560,1440,60,0),
    [V4L2_MODEL_VIDEOFORMAT_4096X2160P60]	= V4L2_MODEL_TIMING(4096,2160,60,0),
};
static const framegrabber_pixfmt_t support_pixfmts[] = {
	
	[FRAMEGRABBER_PIXFMT_YUYV]={ //YUYV index=0
		.name     = "4:2:2, packed, YUYV",
		.fourcc   = V4L2_PIX_FMT_YUYV,
		.depth    = 16,
		.is_yuv   = true,
		.pixfmt_out = YUYV,
	},
	#if 0
	[FRAMEGRABBER_PIXFMT_UYVY]={ //UYVY
		.name     = "4:2:2, packed, UYVY",
		.fourcc   = V4L2_PIX_FMT_UYVY,
		.depth    = 16,
		.is_yuv   = true,
		.pixfmt_out = UYVY,
	},
	[FRAMEGRABBER_PIXFMT_YVYU]={ //YVYU
		.name     = "4:2:2, packed, YVYU",
		.fourcc   = V4L2_PIX_FMT_YVYU,
		.depth    = 16,
		.is_yuv   = true,
		.pixfmt_out = YVYU,
	},
	
	[FRAMEGRABBER_PIXFMT_VYUY]={ //VYUY
		.name     = "4:2:2, packed, VYUY",
		.fourcc   = V4L2_PIX_FMT_VYUY,
		.depth    = 16,
		.is_yuv   = true,
		.pixfmt_out = VYUY,
	},

	[FRAMEGRABBER_PIXFMT_RGB565]={ //RGBP
		.name     = "RGB565 (LE)",
		.fourcc   = V4L2_PIX_FMT_RGB565, /* gggbbbbb rrrrrggg */
		.depth    = 16,
		.is_yuv   = false,
		.pixfmt_out = RGBP,
	},
	[FRAMEGRABBER_PIXFMT_RGB565X]={ //RGBR
		.name     = "RGB565 (BE)",
		.fourcc   = V4L2_PIX_FMT_RGB565X, /* rrrrrggg gggbbbbb */
		.depth    = 16,
		.is_yuv   = false,
		.pixfmt_out = RGBR,
	},
	[FRAMEGRABBER_PIXFMT_RGB555]={ //RGBO
		.name     = "RGB555 (LE)",
		.fourcc   = V4L2_PIX_FMT_RGB555, /* gggbbbbb arrrrrgg */
		.depth    = 16,
		.is_yuv   = false,
		.pixfmt_out = RGBO,
	},
	[FRAMEGRABBER_PIXFMT_RGB555X]={ //RGBQ
		.name     = "RGB555 (BE)",
		.fourcc   = V4L2_PIX_FMT_RGB555X, /* arrrrrgg gggbbbbb */
		.depth    = 16,
		.is_yuv   = false,
		.pixfmt_out = RGBQ,
	},
	[FRAMEGRABBER_PIXFMT_RGB24]={ //RGB3 index=8
		.name     = "RGB24 (LE)",
		.fourcc   = V4L2_PIX_FMT_RGB24, /* rgb */
		.depth    = 24,
		.is_yuv   = false,
		.pixfmt_out = RGB3,
	},

	[FRAMEGRABBER_PIXFMT_BGR24]={ //BGR3
		.name     = "RGB24 (BE)",
		.fourcc   = V4L2_PIX_FMT_BGR24, /* bgr */
		.depth    = 24,
		.is_yuv   = false,
		.pixfmt_out = BGR3,
	},
	[FRAMEGRABBER_PIXFMT_RGB32]={ //RGB4
		.name     = "RGB32 (LE)",
		.fourcc   = V4L2_PIX_FMT_RGB32, /* argb */
		.depth    = 32,
		.is_yuv   = false,
		.pixfmt_out = RGB4,
	},
	[FRAMEGRABBER_PIXFMT_BGR32]={ //BGR4
		.name     = "RGB32 (BE)",
		.fourcc   = V4L2_PIX_FMT_BGR32, /* bgra */
		.depth    = 32,
		.is_yuv   = false,
		.pixfmt_out = BGR4,
	},
	#endif
};

static const int framegrabber_support_refreshrate[]= {
    [REFRESHRATE_15]=15,
    [REFRESHRATE_24]=24,
    [REFRESHRATE_25]=25,
    [REFRESHRATE_30]=30,
    [REFRESHRATE_50]=50,
    [REFRESHRATE_60]=60,
    [REFRESHRATE_100]=100,
    [REFRESHRATE_120]=120,
    [REFRESHRATE_144]=144,
    [REFRESHRATE_240]=240,
};
#define NUM_FRAMERATE_CONTROLS (ARRAY_SIZE(framegrabber_support_refreshrate))

#define HWS_NS_PER_SEC 1000000000ULL

static int hws_nearest_supported_fps(int fps)
{
	int i;
	int best = framegrabber_support_refreshrate[0];
	int best_diff = abs(fps - best);

	for (i = 1; i < NUM_FRAMERATE_CONTROLS; i++) {
		int cur = framegrabber_support_refreshrate[i];
		int diff = abs(fps - cur);
		if (diff < best_diff) {
			best = cur;
			best_diff = diff;
		}
	}
	return best;
}

static int hws_effective_source_fps(struct hws_video *videodev)
{
	u64 avg_ns = READ_ONCE(hws_source_interval_ns_avg[videodev->index]);
	int fps;

	if (!avg_ns)
		return 0;

	fps = (int)DIV_ROUND_CLOSEST_ULL(HWS_NS_PER_SEC, avg_ns);
	if (fps <= 0)
		return 0;

	return hws_nearest_supported_fps(fps);
}
static const int hws_common_fps[] = { 25, 30, 50, 60, 100 };
#define HWS_COMMON_FPS_COUNT ARRAY_SIZE(hws_common_fps)

static bool hws_is_common_fps(int fps)
{
	int i;

	for (i = 0; i < HWS_COMMON_FPS_COUNT; i++) {
		if (hws_common_fps[i] == fps)
			return true;
	}
	return false;
}

static int hws_nearest_common_fps(int fps)
{
	int i;
	int best = hws_common_fps[0];
	int best_diff = abs(fps - best);

	for (i = 1; i < HWS_COMMON_FPS_COUNT; i++) {
		int cur = hws_common_fps[i];
		int diff = abs(fps - cur);
		if (diff < best_diff) {
			best = cur;
			best_diff = diff;
		}
	}
	return best;
}

static int hws_effective_policy_mode(void)
{
	int mode = fps_policy_mode;

	if (mode < HWS_FPS_POLICY_SOURCE_TRUTH || mode > HWS_FPS_POLICY_AUTO_PROFILE)
		mode = HWS_FPS_POLICY_AUTO_PROFILE;

	if (mode == HWS_FPS_POLICY_DRIVER_CONVERT_EXPERIMENTAL && !fps_policy_allow_experimental) {
		pr_warn_ratelimited("hws: fps_policy_mode=3 blocked (experimental disabled), falling back to mode 4\n");
		mode = HWS_FPS_POLICY_AUTO_PROFILE;
	}

	return mode;
}

static int hws_policy_select_fps(struct hws_video *videodev, int fallback_fps)
{
	int src_fps = hws_effective_source_fps(videodev);
	int req_fps = videodev->current_out_framerate;
	int fps;

	switch (hws_effective_policy_mode()) {
	case HWS_FPS_POLICY_SOURCE_TRUTH:
		fps = (src_fps > 0) ? src_fps : req_fps;
		if (fps <= 0)
			fps = fallback_fps;
		return hws_nearest_common_fps(fps);

	case HWS_FPS_POLICY_USERSPACE_CONVERT:
	case HWS_FPS_POLICY_DRIVER_CONVERT_EXPERIMENTAL:
		fps = req_fps;
		if (fps <= 0)
			fps = (src_fps > 0) ? src_fps : fallback_fps;
		if (!hws_is_common_fps(fps))
			fps = hws_nearest_common_fps(fps);
		return fps;

	case HWS_FPS_POLICY_AUTO_PROFILE:
	default:
		if (src_fps > 0) {
			fps = src_fps;
			if (req_fps > 0 && hws_is_common_fps(req_fps) && req_fps < src_fps)
				fps = req_fps;
		} else {
			fps = (req_fps > 0) ? req_fps : fallback_fps;
		}
		if (!hws_is_common_fps(fps))
			fps = hws_nearest_common_fps(fps);
		return fps;
	}
}
//-------------------------------------------
static int v4l2_get_suport_VideoFormatIndex(struct v4l2_format *fmt) 
{
	struct v4l2_pix_format *pix = &fmt->fmt.pix;
	int index;
	int videoIndex=-1;
	for(index=0; index<V4L2_MODEL_VIDEOFORMAT_NUM;index++)
	{
		if((pix->width==support_videofmt[index].frame_size.width)&&(pix->height==support_videofmt[index].frame_size.height))
		{
			videoIndex = index;
			break;
		}
	}
	return videoIndex;
}
static v4l2_model_timing_t *v4l2_model_get_support_videoformat(int index)
{
	if(index <0 ||index >=V4L2_MODEL_VIDEOFORMAT_NUM)
			return NULL;

	return (v4l2_model_timing_t *)&support_videofmt[index];
}


static framegrabber_pixfmt_t *v4l2_model_get_support_pixformat(int index)
{
	if(index <0 ||index >=ARRAY_SIZE(support_pixfmts))
			return NULL;

	return (framegrabber_pixfmt_t *)&support_pixfmts[index];
}
static const framegrabber_pixfmt_t *framegrabber_g_support_pixelfmt_by_fourcc(u32 fourcc)
{
	int i;
	int pixfmt_index=-1;
	for(i=0;i<FRAMEGRABBER_PIXFMT_MAX;i++)
	{
		if(support_pixfmts[i].fourcc==fourcc)
		{
			
			pixfmt_index=i;
			break;
		}
	}
	if(pixfmt_index==-1)
			return NULL;

	return &support_pixfmts[pixfmt_index];
}

static int hws_vidioc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct hws_video *videodev = video_drvdata(file);
	struct hws_pcie_dev *dev = videodev->dev;
	int vi_index;
	vi_index = videodev->index+1+dev->m_Device_PortID*dev->m_nCurreMaxVideoChl;
	//printk( "%s\n", __func__);
	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	scnprintf(cap->card, sizeof(cap->card), "%s %d", HWS_VIDEO_NAME, vi_index);
	strscpy(cap->bus_info, "HWS", sizeof(cap->bus_info));
	cap->device_caps =	V4L2_CAP_VIDEO_CAPTURE |V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	//printk( "%s(IN END  )\n", __func__);
	return 0;
}
static int hws_vidioc_enum_fmt_vid_cap(struct file *file, void *priv_fh,struct v4l2_fmtdesc *f)
{
	struct hws_video *videodev = video_drvdata(file);
	int index = f->index;
	//printk( "%s(%d)\n", __func__,videodev->index);
	//printk( "%s(f->index = %d)\n", __func__,f->index);
	
	if(f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
	{
		if (hws_diag_enabled())
			pr_info_ratelimited("hws: %s invalid buf type=%u\n", __func__, f->type);
		return -EINVAL;
	}
	if(videodev)
	{
		const framegrabber_pixfmt_t *pixfmt;
		if(f->index <0)
		{
			return -EINVAL;
		}
		if(f->index >= FRAMEGRABBER_PIXFMT_MAX)
		{
			return -EINVAL;
		}
		else
		{
			 pixfmt=v4l2_model_get_support_pixformat(f->index);
			 if(pixfmt ==NULL) return -EINVAL;
		    //printk("%s..pixfmt=%d.\n",__func__,f->index);
		    f->index = index;
		    f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		    strscpy(f->description, pixfmt->name, sizeof(f->description));
		    f->pixelformat=pixfmt->fourcc;
		}
	}
	return 0;
}
static const framegrabber_pixfmt_t *framegrabber_g_out_pixelfmt(struct hws_video *dev)
{
	return &support_pixfmts[dev->current_out_pixfmt];
}
static int hws_vidioc_g_fmt_vid_cap(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct hws_video *videodev = video_drvdata(file);
	//struct v4l2_pix_format *pix = &fmt->fmt.pix;
	const framegrabber_pixfmt_t *pixfmt;
	v4l2_model_timing_t *p_SupportmodeTiming;
	//printk( "%s(%d)\n", __func__,videodev->index);
	//printk( "w=%d,h=%d\n",fmt->fmt.pix.width,fmt->fmt.pix.height);
	pixfmt=framegrabber_g_out_pixelfmt(videodev);
	if(pixfmt)
	{
		//framegrabber_g_Curr_input_framesize(videodev,&width,&height);
		p_SupportmodeTiming = v4l2_model_get_support_videoformat(videodev->current_out_size_index);
		if(p_SupportmodeTiming ==NULL) return -EINVAL;
		fmt->fmt.pix.width=p_SupportmodeTiming->frame_size.width;
		fmt->fmt.pix.height=p_SupportmodeTiming->frame_size.height;
		fmt->fmt.pix.field=V4L2_FIELD_NONE; //Field
		fmt->fmt.pix.pixelformat  = pixfmt->fourcc;
		fmt->fmt.pix.bytesperline = (fmt->fmt.pix.width * pixfmt->depth) >> 3;
		fmt->fmt.pix.sizeimage =	fmt->fmt.pix.height * fmt->fmt.pix.bytesperline;
		fmt->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
		//printk("%s....f->fmt.pix.width=%d.f->fmt.pix.height=%d.\n",__func__,fmt->fmt.pix.width,fmt->fmt.pix.height);
		return 0;
	} 

	return -EINVAL;
}
static v4l2_model_timing_t *Get_input_framesizeIndex(int width,int height)
{	
	int i;
	for(i =0;i<V4L2_MODEL_VIDEOFORMAT_NUM;i++)
	{
		if((support_videofmt[i].frame_size.width ==width)&&(support_videofmt[i].frame_size.height == height))
		{
			return (v4l2_model_timing_t *)&support_videofmt[i];
		}
	}
	return NULL;
}

static int hws_vidioc_try_fmt_vid_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct hws_video *videodev = video_drvdata(file);
	v4l2_model_timing_t *pModeTiming;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	const framegrabber_pixfmt_t *fmt;
	//int TimeingIndex = f->index;
	//printk( "%s(%d)\n", __func__,videodev->index);
	//printk( "pix->height =%d  pix->width =%d \n", pix->height,pix->width);
 
    if (pix->pixelformat == 0) {
        if (framegrabber_g_support_pixelfmt_by_fourcc(videodev->pixfmt))
            pix->pixelformat = videodev->pixfmt;
        else
            pix->pixelformat = V4L2_PIX_FMT_YUYV;
    }
    
	fmt = framegrabber_g_support_pixelfmt_by_fourcc(pix->pixelformat);
	if(!fmt)
 {
        pix->pixelformat = V4L2_PIX_FMT_YUYV;
        fmt = framegrabber_g_support_pixelfmt_by_fourcc(pix->pixelformat);
        if (!fmt) {
            if (hws_diag_enabled())
                pr_info_ratelimited("hws: %s unsupported format fourcc=0x%x\n",
                                    __func__, pix->pixelformat);
            return -EINVAL;
        }
	}
 if (pix->width == 0 || pix->height == 0) {
        pModeTiming = v4l2_model_get_support_videoformat(videodev->current_out_size_index);
        if (!pModeTiming) return -EINVAL;
        pix->width  = pModeTiming->frame_size.width;
        pix->height = pModeTiming->frame_size.height;
    }
	pModeTiming = Get_input_framesizeIndex(pix->width,pix->height);
	if(!pModeTiming)
	{
		if (hws_diag_enabled())
			pr_info_ratelimited("hws: %s unsupported size %ux%u, falling back\n",
					    __func__, pix->width, pix->height);
		pModeTiming = v4l2_model_get_support_videoformat(videodev->current_out_size_index);
		if(pModeTiming ==NULL) return -EINVAL;
		pix->field = V4L2_FIELD_NONE;
		pix->width=pModeTiming->frame_size.width;
		pix->height=pModeTiming->frame_size.height;
		pix->bytesperline = (pix->width * fmt->depth) >> 3;
		pix->sizeimage =pix->height * pix->bytesperline; 
		pix->colorspace = V4L2_COLORSPACE_REC709;//V4L2_COLORSPACE_SMPTE170M;
		pix->priv = 0;
        //return -EINVAL;
		return 0;
	}
	pix->field = V4L2_FIELD_NONE;
	pix->width=pModeTiming->frame_size.width;
	pix->height=pModeTiming->frame_size.height;
	pix->bytesperline = (pix->width * fmt->depth) >> 3;
	pix->sizeimage =pix->height * pix->bytesperline; 
	pix->colorspace = V4L2_COLORSPACE_REC709;//V4L2_COLORSPACE_SMPTE170M;
	pix->priv = 0;
	
   //printk("%s<<pix->width=%d.pix->height=%d.\n",__func__,pix->width,pix->height);      
	return 0;


//----------------------------------
	return 0;
}
static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,struct v4l2_format *f)
{
	struct hws_video *videodev = video_drvdata(file);
	int nVideoFmtIndex;
	int err;
	unsigned long flags;
	struct hws_pcie_dev *pdx = videodev->dev;
	//printk( "%s()\n", __func__);
	err = hws_vidioc_try_fmt_vid_cap(file, priv, f);
	if (err)
		return err;

	nVideoFmtIndex = v4l2_get_suport_VideoFormatIndex(f);
	if (nVideoFmtIndex == -1)
		return -EINVAL;
	spin_lock_irqsave(&pdx->videoslock[videodev->index], flags);	
	videodev->current_out_size_index = nVideoFmtIndex;
	videodev->pixfmt     = f->fmt.pix.pixelformat;
	videodev->current_out_width      = f->fmt.pix.width;
	videodev->curren_out_height     = f->fmt.pix.height;
	//printk("%s<<  current_out_size_index =%d current_out_width=%d.curren_out_height=%d.\n",__func__,videodev->current_out_size_index,videodev->current_out_width ,videodev->curren_out_height );
	spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);
		return 0;
}
static int hws_vidioc_g_std(struct file *file, void *priv, v4l2_std_id *tvnorms)
{
	struct hws_video *videodev = video_drvdata(file);
	//printk( "%s()\n", __func__);
	*tvnorms = videodev->std;
	return 0;
}

static int hws_vidioc_s_std(struct file *file, void *priv,v4l2_std_id tvnorms)
{
	struct hws_video *videodev = video_drvdata(file);
	//printk( "%s()\n", __func__);
	videodev->std = tvnorms;
	return 0;
}

static int hws_vidioc_g_parm(struct file *file, void *fh, struct v4l2_streamparm *setfps)
{
	struct hws_video *videodev = video_drvdata(file);
	v4l2_model_timing_t *mode;
	int fallback_fps;
	int fps;

	if (setfps->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	mode = v4l2_model_get_support_videoformat(videodev->current_out_size_index);
	fallback_fps = mode ? mode->refresh_rate : 60;
	if (fallback_fps <= 0)
		fallback_fps = 60;

	fps = hws_policy_select_fps(videodev, fallback_fps);

	setfps->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	setfps->parm.capture.timeperframe.numerator = 1;
	setfps->parm.capture.timeperframe.denominator = fps;
	setfps->parm.capture.readbuffers = 0;
	return 0;
}
static int hws_vidioc_enum_framesizes(struct file *file, void *fh, struct v4l2_frmsizeenum *fsize)
{
	//struct hws_video *videodev = video_drvdata(file);
	const framegrabber_pixfmt_t *pixfmt;
	v4l2_model_timing_t *p_SupportmodeTiming;
	int width=0,height=0;
		//printk( "%s(%d)-FrameIndex=[%d]\n", __func__,videodev->index,fsize->index);
	//----------------------------
	pixfmt=framegrabber_g_support_pixelfmt_by_fourcc(fsize->pixel_format);
	if(pixfmt==NULL)
	{
		//printk("%s..\n",__func__);
		return -EINVAL;
	}
	p_SupportmodeTiming = v4l2_model_get_support_videoformat(fsize->index);
	if(p_SupportmodeTiming == NULL)
	{
		//printk("%s. invalid framesize[%d]\n",__func__,fsize->index);
		return -EINVAL;
	}
	width = p_SupportmodeTiming->frame_size.width;
	height = p_SupportmodeTiming->frame_size.height;
	
	//printk("%s...supportframesize[%d] width=%d height=%d Framerate=%d..\n",__func__,fsize->index,width,height,frameRate); //12
	if((width ==0) || (height ==0))
	{
		//printk("%s. invalid framesize 2\n",__func__);
		return -EINVAL;
	}
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->pixel_format=pixfmt->fourcc;
	fsize->discrete.width=width;
	fsize->discrete.height=height;
	//fsize->discrete.denominator = frameRate;
	//fsize->discrete..numerator =1 ;
	//-------------------------------
	//width = videodev->current_out_width;
	//height = videodev->curren_out_height;
	//fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	//fsize->pixel_format=videodev->pixfmt;
	//fsize->discrete.width=width;
	//fsize->discrete.height=height;
	return 0;

	
}

static int hws_vidioc_enum_input(struct file *file, void *priv,struct v4l2_input *i)
{
	//struct hws_video *videodev = video_drvdata(file);
	int Index;
	Index = i->index; 
	//printk( "%s(%d)-%d Index =%d \n", __func__,videodev->index,i->index,Index);
	if(Index >0)
	{
		return   -EINVAL;
	}
	i->type = V4L2_INPUT_TYPE_CAMERA;
	strscpy(i->name, KBUILD_MODNAME, sizeof(i->name));
	i->std = V4L2_STD_NTSC_M;
	i->capabilities = 0;
	i->status=0;
	
	return 0;
}

static int hws_vidioc_g_input(struct file *file, void *priv, unsigned int *i)
{
	//struct hws_video *videodev = video_drvdata(file);
	int Index;
	Index = *i;
	//printk( "%s(%d)-index =%d\n", __func__,videodev->index,Index);
	
	#if 0
	if(Index <0 ||Index >=V4L2_MODEL_VIDEOFORMAT_NUM)
	{
		return   -EINVAL;
	}
	else
	{
		*i = Index;
	}
	#else
	if(Index >0)
	{
		return   -EINVAL;
	}
	else
	{
		*i = 0;
	}	
	#endif 
	return 0;
}

static int hws_vidioc_s_input(struct file *file, void *priv, unsigned int i)
{
	#if 0
	struct hws_video *videodev = video_drvdata(file);

	int Index;
	v4l2_model_timing_t *p_SupportmodeTiming;
	Index = i;
	if(Index <0 ||Index >=V4L2_MODEL_VIDEOFORMAT_NUM)
	{
		return   -EINVAL;
	}
	p_SupportmodeTiming = v4l2_model_get_support_videoformat(Index);
	videodev->current_out_size_index = Index;
	videodev->current_out_width = p_SupportmodeTiming->frame_size.width;
	videodev->curren_out_height = p_SupportmodeTiming->frame_size.height;
	printk( "%s(%d)- %dx%d \n", __func__,i,videodev->current_out_width,videodev->curren_out_height);
	#endif 
	//printk( "%s(%d)\n", __func__,i);
	return i ? -EINVAL : 0;
}
static int hws_vidioc_log_status(struct file *file, void *priv)
{
	/*
	 * Avoid START/END STATUS banner spam unless diagnostics were explicitly
	 * enabled by module parameter.
	 */
	if (!hws_diag_enabled())
		return -ENOTTY;
	return 0;
}

static ssize_t hws_read(struct file *file,char *buf,size_t count, loff_t *ppos)
{
	//printk( "%s()\n", __func__);
	return -1;
		
}

static inline struct hws_vfh_ctx *hws_ctx_from_file(struct file *file)
{
    return container_of(file->private_data, struct hws_vfh_ctx, fh);
}

static bool hws_cmd_requires_exclusive_owner(unsigned int cmd)
{
    switch (cmd) {
    case VIDIOC_S_FMT:
    case VIDIOC_REQBUFS:
    case VIDIOC_CREATE_BUFS:
    case VIDIOC_STREAMON:
    case VIDIOC_STREAMOFF:
        return true;
    default:
        return false;
    }
}

/* B1 multi-consumer vb2 ops (per-file queue). Defined later in this file. */
static const struct vb2_ops hwspcie_video_multi_qops;

static int hws_open(struct file *file)
{
    struct hws_video *videodev = video_drvdata(file);
    struct hws_pcie_dev *pdx = videodev->dev;
    unsigned long flags;
    struct hws_vfh_ctx *ctx;
    struct vb2_queue *q;
    int ret;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    ctx->video = videodev;
    INIT_LIST_HEAD(&ctx->buf_queue);
    spin_lock_init(&ctx->qlock);
    ctx->streaming = false;
    WRITE_ONCE(videodev->next_frame_ts_ns, 0);
	videodev->output_rate_accum = 0;
    ctx->seqnr = 0;

    /* v4l2 file-handle */
    v4l2_fh_init(&ctx->fh, &videodev->vdev);
    file->private_data = &ctx->fh;
    HWS_V4L2_FH_ADD(&ctx->fh, file);

    /* per-file vb2 queue */
    q = &ctx->vbq;
    memset(q, 0, sizeof(*q));
    q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    q->io_modes = VB2_READ | VB2_MMAP | VB2_USERPTR;
    q->gfp_flags = GFP_KERNEL | __GFP_ZERO;//GFP_DMA32;
    q->drv_priv = ctx;
    q->buf_struct_size = sizeof(struct hwsvideo_buffer);
    q->ops = &hwspcie_video_multi_qops;
    q->mem_ops = &vb2_vmalloc_memops;
    q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    q->lock = NULL; /* we use our own locks */
    q->dev = &pdx->pdev->dev;

    ret = vb2_queue_init(q);
    if (ret) {
        HWS_V4L2_FH_DEL(&ctx->fh,file);
        v4l2_fh_exit(&ctx->fh);
        kfree(ctx);
        file->private_data = NULL;
        return ret;
    }
    spin_lock_irqsave(&pdx->videoslock[videodev->index], flags);
    videodev->fileindex++;
    spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);

    /* add to consumers list */
    spin_lock_irqsave(&videodev->consumers_lock, flags);
    list_add_tail(&ctx->node, &videodev->consumers);
    spin_unlock_irqrestore(&videodev->consumers_lock, flags);

    return 0;
}

static int hws_release(struct file *file)
{
    struct hws_vfh_ctx *ctx;
    struct hws_video *videodev;
    struct hws_pcie_dev *pdx;
    unsigned long flags;

    if (!file->private_data)
        return 0;

    ctx = hws_ctx_from_file(file);
    videodev = ctx->video;
    pdx = videodev->dev;
    spin_lock_irqsave(&pdx->videoslock[videodev->index], flags);
    if (videodev->fileindex > 0)
        videodev->fileindex--;
    spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);

    /*
     * Ensure no in-flight videowork still references this ctx while we
     * tear down vb2 resources.
     */
    flush_work(&videodev->videowork);

    /* remove from consumers */
    spin_lock_irqsave(&videodev->consumers_lock, flags);
    list_del(&ctx->node);
    spin_unlock_irqrestore(&videodev->consumers_lock, flags);

    /*
     * Single-owner mode: if this fd owned stream-affecting ioctls, release
     * ownership on close so a later opener can claim it.
     */
    mutex_lock(&videodev->ioctl_lock);
    if (videodev->ioctl_owner == ctx)
        videodev->ioctl_owner = NULL;
    mutex_unlock(&videodev->ioctl_lock);

    /* release vb2 queue resources */
    vb2_queue_release(&ctx->vbq);

    /* v4l2 fh cleanup */
    HWS_V4L2_FH_DEL(&ctx->fh,file);
    v4l2_fh_exit(&ctx->fh);
    file->private_data = NULL;

    kfree(ctx);
    return 0;
}

//-------------------
static const struct v4l2_queryctrl __maybe_unused g_no_ctrl = {
	.name  = "42",
	.flags = V4L2_CTRL_FLAG_DISABLED,
};
static const struct v4l2_queryctrl __maybe_unused g_hws_ctrls[] =
{
	#if 1
	{
		V4L2_CID_BRIGHTNESS,           //id
		V4L2_CTRL_TYPE_INTEGER,        //type
		"Brightness",                  //name[32]
		MIN_VAMP_BRIGHTNESS_UNITS,     //minimum
		MAX_VAMP_BRIGHTNESS_UNITS,     //maximum
        1,                             //step
		BrightnessDefault,             //default_value
		0,                             //flags
	    { 0, 0 },                      //reserved[2]
	},
	{
		V4L2_CID_CONTRAST,             //id
		V4L2_CTRL_TYPE_INTEGER,        //type
		"Contrast",                    //name[32]
		MIN_VAMP_CONTRAST_UNITS,       //minimum
		MAX_VAMP_CONTRAST_UNITS,       //maximum
        1,                             //step
		ContrastDefault,               //default_value
		0,                             //flags
	    { 0, 0 },                      //reserved[2]
	},
	{
		V4L2_CID_SATURATION,           //id
		V4L2_CTRL_TYPE_INTEGER,        //type
		"Saturation",                  //name[32]
		MIN_VAMP_SATURATION_UNITS,     //minimum
		MAX_VAMP_SATURATION_UNITS,     //maximum
        1,                             //step
		SaturationDefault,             //default_value
		0,                             //flags
	    { 0, 0 },                      //reserved[2]
	},
	{
		V4L2_CID_HUE,                  //id
		V4L2_CTRL_TYPE_INTEGER,        //type
		"Hue",                         //name[32]
		MIN_VAMP_HUE_UNITS,            //minimum
		MAX_VAMP_HUE_UNITS,            //maximum
        1,                             //step
		HueDefault,                    //default_value
		0,                             //flags
	    { 0, 0 },                      //reserved[2]
	},
	#endif
	#if 0
	{
		V4L2_CID_AUTOGAIN,           //id
		V4L2_CTRL_TYPE_INTEGER,        //type
		"Hdcp enable",                 //name[32]
		0,                             //minimum
		1,                             //maximum
		1,                             //step
		0,                             //default_value
		0,                             //flags
		{ 0, 0 },                      //reserved[2]
	},
	{
		V4L2_CID_GAIN,           //id
		V4L2_CTRL_TYPE_INTEGER,        //type
		"Sample rate",                        //name[32]
		48000,                             //minimum
		48000,                             //maximum
		1,                             //step
		48000,                             //default_value
		0,                             //flags
		{ 0, 0 },                      //reserved[2]
	}
	#endif
};

#define ARRAY_SIZE_OF_CTRL		(sizeof(g_hws_ctrls)/sizeof(g_hws_ctrls[0]))

static const struct v4l2_queryctrl __maybe_unused *find_ctrlByIndex(unsigned int index)
{
	//scan supported queryctrl table
	if(index>=ARRAY_SIZE_OF_CTRL)
	{
		return NULL;
	}
	else
	{
		return &g_hws_ctrls[index];
	}
}

static const struct v4l2_queryctrl __maybe_unused *find_ctrl(unsigned int id)
{
	int i;
	//scan supported queryctrl table
	for( i=0; i<ARRAY_SIZE_OF_CTRL; i++ )
		if(g_hws_ctrls[i].id==id)
			return &g_hws_ctrls[i];

	return 0;
}

static const struct v4l2_queryctrl __maybe_unused *find_next_ctrl(unsigned int id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE_OF_CTRL; i++) {
		if (g_hws_ctrls[i].id > id)
			return &g_hws_ctrls[i];
	}

	return NULL;
}

static int hws_ctrl_get_value(struct hws_video *videodev, u32 id, s32 *value)
{
	if (!videodev || !value)
		return -EINVAL;

	switch (id) {
	case V4L2_CID_BRIGHTNESS:
		*value = videodev->m_Curr_Brightness;
		return 0;
	case V4L2_CID_CONTRAST:
		*value = videodev->m_Curr_Contrast;
		return 0;
	case V4L2_CID_SATURATION:
		*value = videodev->m_Curr_Saturation;
		return 0;
	case V4L2_CID_HUE:
		*value = videodev->m_Curr_Hue;
		return 0;
	case V4L2_CID_MIN_BUFFERS_FOR_CAPTURE:
		*value = 3;
		return 0;
	case V4L2_CID_MIN_BUFFERS_FOR_OUTPUT:
		*value = 0;
		return 0;
	default:
		return -EINVAL;
	}
}

static int hws_ctrl_set_value(struct hws_video *videodev, u32 id, s32 value)
{
	if (!videodev)
		return -EINVAL;

	switch (id) {
	case V4L2_CID_BRIGHTNESS:
		videodev->m_Curr_Brightness = value;
		return 0;
	case V4L2_CID_CONTRAST:
		videodev->m_Curr_Contrast = value;
		return 0;
	case V4L2_CID_HUE:
		videodev->m_Curr_Hue = value;
		return 0;
	case V4L2_CID_SATURATION:
		videodev->m_Curr_Saturation = value;
		return 0;
	default:
		return -EINVAL;
	}
}
//-----------------------------

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,13,0)
static const struct v4l2_query_ext_ctrl g_hws_ext_ctrls[] = {
    {
        .id = V4L2_CID_BRIGHTNESS,
        .type = V4L2_CTRL_TYPE_INTEGER,
        .name = "Brightness",
        .minimum = MIN_VAMP_BRIGHTNESS_UNITS,
        .maximum = MAX_VAMP_BRIGHTNESS_UNITS,
        .step = 1,
        .default_value = BrightnessDefault,
        .flags = 0,
        .elem_size = sizeof(s32),
        .dims = {0},
        .nr_of_dims = 0,
    },
    {
        .id = V4L2_CID_CONTRAST,
        .type = V4L2_CTRL_TYPE_INTEGER,
        .name = "Contrast",
        .minimum = MIN_VAMP_CONTRAST_UNITS,
        .maximum = MAX_VAMP_CONTRAST_UNITS,
        .step = 1,
        .default_value = ContrastDefault,
        .flags = 0,
        .elem_size = sizeof(s32),
        .dims = {0},
        .nr_of_dims = 0,
    },
    {
        .id = V4L2_CID_SATURATION,
        .type = V4L2_CTRL_TYPE_INTEGER,
        .name = "Saturation",
        .minimum = MIN_VAMP_SATURATION_UNITS,
        .maximum = MAX_VAMP_SATURATION_UNITS,
        .step = 1,
        .default_value = SaturationDefault,
        .flags = 0,
        .elem_size = sizeof(s32),
        .dims = {0},
        .nr_of_dims = 0,
    },
    {
        .id = V4L2_CID_HUE,
        .type = V4L2_CTRL_TYPE_INTEGER,
        .name = "Hue",
        .minimum = MIN_VAMP_HUE_UNITS,
        .maximum = MAX_VAMP_HUE_UNITS,
        .step = 1,
        .default_value = HueDefault,
        .flags = 0,
        .elem_size = sizeof(s32),
        .dims = {0},
        .nr_of_dims = 0,
    },
};
#define ARRAY_SIZE_OF_EXT_CTRL (sizeof(g_hws_ext_ctrls) / sizeof(g_hws_ext_ctrls[0]))
static const struct v4l2_query_ext_ctrl *find_ext_ctrl(unsigned int id)
{
    int i;
    for (i = 0; i < ARRAY_SIZE_OF_EXT_CTRL; i++) {
        if (g_hws_ext_ctrls[i].id == id) {
            return &g_hws_ext_ctrls[i];
        }
    }
    return NULL;
}

static const struct v4l2_query_ext_ctrl *find_next_ext_ctrl(unsigned int id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE_OF_EXT_CTRL; i++) {
		if (g_hws_ext_ctrls[i].id > id)
			return &g_hws_ext_ctrls[i];
	}

	return NULL;
}
//-------------------------
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,13,0)
static int hws_vidioc_g_ctrl(struct file *file, void *fh,struct v4l2_control *a)//
{
	struct hws_video *videodev = video_drvdata(file);
	struct v4l2_control *ctrl = a;
	int ret = -EINVAL;
	if(ctrl ==NULL)
	{
		if (hws_diag_enabled())
			pr_info_ratelimited("hws: %s ch=%d ctrl is NULL\n", __func__, videodev->index);
		return ret;
	}
	ret = hws_ctrl_get_value(videodev, ctrl->id, &ctrl->value);
	if (ret < 0) {
		ctrl->value = 0;
		if (hws_diag_enabled())
			pr_info_ratelimited("hws: g_ctrl unsupported id=0x%x\n", ctrl->id);
	}
	return ret;

}
#else 
static int hws_v4l2_g_ext_ctrls(struct file *file, void *fh,struct v4l2_ext_controls  *cs)//
{
	struct hws_video *videodev = video_drvdata(file);
	int i;
	if(cs ==NULL)
	{
		if (hws_diag_enabled())
			pr_info_ratelimited("hws: %s ch=%d ext ctrls are NULL\n", __func__, videodev->index);
		return -EINVAL;
	}
	cs->error_idx = cs->count;
	for (i = 0; i < cs->count; i++) {
		struct v4l2_ext_control *c = &cs->controls[i];
		int ret = hws_ctrl_get_value(videodev, c->id, &c->value);

		if (ret < 0) {
			cs->error_idx = i;
			if (hws_diag_enabled())
				pr_info_ratelimited("hws: g_ext_ctrls unsupported id=0x%x\n", c->id);
			return ret;
		}
	}
	return 0;

}
#endif 
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,13,0)
static int hws_vidioc_s_ctrl(struct file *file, void *fh,struct v4l2_control *a)
{
	struct hws_video *videodev = video_drvdata(file);
	struct v4l2_control *ctrl = a;
	const struct v4l2_queryctrl *found_ctrl;
	int ret = -EINVAL;
	if(ctrl ==NULL)
	{
		if (hws_diag_enabled())
			pr_info_ratelimited("hws: %s ch=%d ctrl is NULL\n", __func__, videodev->index);
		return ret;
	}
	//printk( "%s(ch-%d ctrl->id =%X )\n", __func__,videodev->index,ctrl->id);
	found_ctrl = find_ctrl(ctrl->id);
	if( found_ctrl ) {
		switch( found_ctrl->type ) {
		case V4L2_CTRL_TYPE_INTEGER:
			if (ctrl->value >= found_ctrl->minimum &&
			    ctrl->value <= found_ctrl->maximum) {
				ret = hws_ctrl_set_value(videodev, ctrl->id, ctrl->value);
			}
			else 
			{
				//error
				ret = -ERANGE;
				if (hws_diag_enabled())
					pr_info_ratelimited("hws: s_ctrl out of range: %s\n", found_ctrl->name);
			}
			break;
		default:
		{
			//error
			if (hws_diag_enabled())
					pr_info_ratelimited("hws: s_ctrl unsupported type=%d\n", found_ctrl->type);
			}
			
		}
	}
	//printk( "%s(ret=%d)\n", __func__,ret);
	return ret;

}
#else 
static int hws_v4l2_s_ext_ctrls(struct file *file, void *fh,struct v4l2_ext_controls  *cs)
{
	struct hws_video *videodev = video_drvdata(file);
	int i;
	if(cs ==NULL)
	{
		if (hws_diag_enabled())
			pr_info_ratelimited("hws: %s ch=%d ext ctrls are NULL\n", __func__, videodev->index);
		return -EINVAL;
	}
	cs->error_idx = cs->count;
	for (i = 0; i < cs->count; i++) {
		struct v4l2_ext_control *c = &cs->controls[i];
		const struct v4l2_query_ext_ctrl *found_ctrl;
		int ret;

		found_ctrl = find_ext_ctrl(c->id);
		if (!found_ctrl) {
			cs->error_idx = i;
			if (hws_diag_enabled())
				pr_info_ratelimited("hws: s_ext_ctrls unsupported id=0x%x\n", c->id);
			return -EINVAL;
		}

		if (c->value < found_ctrl->minimum || c->value > found_ctrl->maximum) {
			cs->error_idx = i;
			if (hws_diag_enabled())
				pr_info_ratelimited("hws: s_ext_ctrls out of range id=0x%x value=%d range=%lld..%lld\n",
						    c->id, c->value,
						    found_ctrl->minimum, found_ctrl->maximum);
			return -ERANGE;
		}

		ret = hws_ctrl_set_value(videodev, c->id, c->value);
		if (ret < 0) {
			cs->error_idx = i;
			if (hws_diag_enabled())
				pr_info_ratelimited("hws: s_ext_ctrls unsupported id=0x%x\n", c->id);
			return ret;
		}
	}
	return 0;

}
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(6,13,0)
static int hws_vidioc_queryctrl(struct file *file, void *fh,struct v4l2_queryctrl *a)
{
	const struct v4l2_queryctrl *found_ctrl;
	unsigned int id;
	unsigned int mask_id;

	id = a->id & (~V4L2_CTRL_FLAG_NEXT_CTRL);
	mask_id = a->id & V4L2_CTRL_FLAG_NEXT_CTRL;
	found_ctrl = (mask_id == V4L2_CTRL_FLAG_NEXT_CTRL) ?
		find_next_ctrl(id) : find_ctrl(id);
	if (found_ctrl == NULL) {
		*a = g_no_ctrl;
		return -EINVAL;
	}

	*a = *found_ctrl;
	return 0;

}
#else
static int hws_v4l2_query_ext_ctrl(struct file *file, void *fh, struct v4l2_query_ext_ctrl *qc)
{
	struct hws_video *videodev = video_drvdata(file);
	const struct v4l2_query_ext_ctrl *found_ctrl;
	unsigned int id;
	unsigned int mask_id;

	if (qc == NULL) {
		if (hws_diag_enabled())
			pr_info_ratelimited("hws: %s ch=%d ext ctrls are NULL\n", __func__, videodev->index);
		return -EINVAL;
	}

	id = qc->id & (~V4L2_CTRL_FLAG_NEXT_CTRL);
	mask_id = qc->id & V4L2_CTRL_FLAG_NEXT_CTRL;
	found_ctrl = (mask_id == V4L2_CTRL_FLAG_NEXT_CTRL) ?
		find_next_ext_ctrl(id) : find_ext_ctrl(id);
	if (found_ctrl == NULL) {
		memset(qc, 0, sizeof(*qc));
		return -EINVAL;
	}

	memcpy(qc, found_ctrl, sizeof(*qc));
	return 0;
}
#endif 
#if 0
static int hws_vidioc_streamon(struct file *file, void *priv, enum v4l2_buf_type i)
{
	//struct hws_video *videodev = video_drvdata(file);
	//printk( "%s(ch-%d)\n", __func__,videodev->index); 
	#if 0
	StartVideoCapture(videodev->dev,videodev->index);
	#endif 
	return(vb2_ioctl_streamon(file,priv,i));
	
}
static int hws_vidioc_streamoff(struct file *file, void *priv, enum v4l2_buf_type i)
{
	//struct hws_video *videodev = video_drvdata(file);
	//printk( "%s(ch-%d)\n", __func__,videodev->index); 
	#if 0
	StopVideoCapture(videodev->dev,videodev->index);
	#endif 
	return(vb2_ioctl_streamoff(file,priv,i));

}
#endif 
static int hws_vidioc_enum_frameintervals(struct file *file, void *fh,
			   struct v4l2_frmivalenum *fival)
{
	struct hws_video *videodev = video_drvdata(file);
	const framegrabber_pixfmt_t *pixfmt;
	v4l2_model_timing_t *mode;
	int fps;
	int src_fps;
	int policy_mode = hws_effective_policy_mode();

	pixfmt = framegrabber_g_support_pixelfmt_by_fourcc(fival->pixel_format);
	if (!pixfmt)
		return -EINVAL;

	mode = Get_input_framesizeIndex(fival->width, fival->height);
	if (!mode)
		return -EINVAL;

	src_fps = hws_effective_source_fps(videodev);
	if (policy_mode == HWS_FPS_POLICY_SOURCE_TRUTH) {
		if (fival->index > 0)
			return -EINVAL;
		fps = hws_policy_select_fps(videodev, mode->refresh_rate);
	} else if (policy_mode == HWS_FPS_POLICY_AUTO_PROFILE) {
		if (fival->index >= HWS_COMMON_FPS_COUNT)
			return -EINVAL;
		fps = hws_common_fps[fival->index];
		if (src_fps > 0 && fps > src_fps)
			return -EINVAL;
	} else {
		if (fival->index >= HWS_COMMON_FPS_COUNT)
			return -EINVAL;
		fps = hws_common_fps[fival->index];
	}

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete.numerator = 1;
	fival->discrete.denominator = fps;
	return 0;
}
static int hws_vidioc_s_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct hws_video *videodev = video_drvdata(file);
	v4l2_model_timing_t *mode;
	int io_frame_rate;
	int src_fps;
	int policy_mode = hws_effective_policy_mode();

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (a->parm.capture.timeperframe.numerator <= 0 ||
	    a->parm.capture.timeperframe.denominator <= 0)
		return -EINVAL;

	io_frame_rate = a->parm.capture.timeperframe.denominator /
			a->parm.capture.timeperframe.numerator;
	if (io_frame_rate <= 0)
		return -EINVAL;

	if (!hws_is_common_fps(io_frame_rate))
		io_frame_rate = hws_nearest_common_fps(io_frame_rate);

	src_fps = hws_effective_source_fps(videodev);
	if (policy_mode == HWS_FPS_POLICY_SOURCE_TRUTH && src_fps > 0) {
		if (abs(io_frame_rate - src_fps) > 1)
			return -EINVAL;
		io_frame_rate = src_fps;
	} else if (policy_mode == HWS_FPS_POLICY_SOURCE_TRUTH) {
		mode = v4l2_model_get_support_videoformat(videodev->current_out_size_index);
		if (mode)
			io_frame_rate = hws_nearest_common_fps(mode->refresh_rate);
	} else if (policy_mode == HWS_FPS_POLICY_AUTO_PROFILE && src_fps > 0) {
		if (io_frame_rate > src_fps)
			io_frame_rate = src_fps;
	}

	videodev->current_out_framerate = io_frame_rate;
	a->parm.capture.timeperframe.numerator = 1;
	a->parm.capture.timeperframe.denominator = io_frame_rate;
	a->parm.capture.readbuffers = 0;

	return 0;
}

/* --- B1 multi-consumer: per-file vb2_queue switching wrappers --- */
static long hws_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct v4l2_fh *fh = file->private_data;
    struct hws_vfh_ctx *ctx;
    struct hws_video *videodev;
    struct vb2_queue *oldq;
    bool needs_owner;
    bool owner_claimed = false;
    long ret;

    if (!fh)
        return -EINVAL;
    ctx = container_of(fh, struct hws_vfh_ctx, fh);
    videodev = ctx->video;

    /*
     * Some userspace stacks aggressively call VIDIOC_LOG_STATUS and can flood
     * the kernel log. Keep it opt-in behind diag_enable.
     */
    if (cmd == VIDIOC_LOG_STATUS && !hws_diag_enabled())
        return -ENOTTY;

    mutex_lock(&videodev->ioctl_lock);

    needs_owner = hws_cmd_requires_exclusive_owner(cmd);
    if (needs_owner) {
        if (videodev->ioctl_owner && videodev->ioctl_owner != ctx) {
            ret = -EBUSY;
            goto out_unlock;
        }
        if (!videodev->ioctl_owner) {
            videodev->ioctl_owner = ctx;
            owner_claimed = true;
        }
    }

    oldq = videodev->vdev.queue;
    videodev->vdev.queue = &ctx->vbq;
    ret = video_ioctl2(file, cmd, arg);
    videodev->vdev.queue = oldq;

    /*
     * If a first claim fails immediately, do not keep stale ownership.
     * STREAMOFF (or close) releases ownership after a successful stop.
     */
    if (ret && owner_claimed)
        videodev->ioctl_owner = NULL;
    else if (!ret && cmd == VIDIOC_STREAMOFF && videodev->ioctl_owner == ctx)
        videodev->ioctl_owner = NULL;

out_unlock:
    mutex_unlock(&videodev->ioctl_lock);

    return ret;
}

static __poll_t hws_poll(struct file *file, struct poll_table_struct *wait)
{
    struct v4l2_fh *fh = file->private_data;
    struct hws_vfh_ctx *ctx;
    struct hws_video *videodev;
    struct vb2_queue *oldq;
    __poll_t ret;

    if (!fh)
        return EPOLLERR;
    ctx = container_of(fh, struct hws_vfh_ctx, fh);
    videodev = ctx->video;

    mutex_lock(&videodev->ioctl_lock);
    oldq = videodev->vdev.queue;
    videodev->vdev.queue = &ctx->vbq;
    ret = vb2_fop_poll(file, wait);
    videodev->vdev.queue = oldq;
    mutex_unlock(&videodev->ioctl_lock);

    return ret;
}

static int hws_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct v4l2_fh *fh = file->private_data;
    struct hws_vfh_ctx *ctx;
    struct hws_video *videodev;
    struct vb2_queue *oldq;
    int ret;

    if (!fh)
        return -EINVAL;
    ctx = container_of(fh, struct hws_vfh_ctx, fh);
    videodev = ctx->video;

    mutex_lock(&videodev->ioctl_lock);
    oldq = videodev->vdev.queue;
    videodev->vdev.queue = &ctx->vbq;
    ret = vb2_fop_mmap(file, vma);
    videodev->vdev.queue = oldq;
    mutex_unlock(&videodev->ioctl_lock);

    return ret;
}

/* forward declaration */
static const struct vb2_ops hwspcie_video_multi_qops;
static const struct v4l2_file_operations hws_fops = {
    .owner          = THIS_MODULE,
    .open           = hws_open,
    .release        = hws_release,
    .read           = hws_read,
    .poll           = hws_poll,
    .unlocked_ioctl = hws_unlocked_ioctl,
    .mmap           = hws_mmap,
};


static const struct v4l2_ioctl_ops hws_ioctl_fops = {
	.vidioc_querycap = hws_vidioc_querycap,
	.vidioc_enum_fmt_vid_cap = hws_vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = hws_vidioc_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = vidioc_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = hws_vidioc_try_fmt_vid_cap,
	.vidioc_reqbufs       = vb2_ioctl_reqbufs,
	.vidioc_prepare_buf   = vb2_ioctl_prepare_buf,
	.vidioc_create_bufs   = vb2_ioctl_create_bufs,
	.vidioc_querybuf      = vb2_ioctl_querybuf,
	.vidioc_qbuf          = vb2_ioctl_qbuf,
	.vidioc_dqbuf         = vb2_ioctl_dqbuf,
	.vidioc_streamon      = vb2_ioctl_streamon,
	.vidioc_streamoff     = vb2_ioctl_streamoff,
	.vidioc_g_std = hws_vidioc_g_std,
	.vidioc_s_std = hws_vidioc_s_std,
	.vidioc_enum_framesizes   	= hws_vidioc_enum_framesizes,
	.vidioc_enum_frameintervals = hws_vidioc_enum_frameintervals,
	#if LINUX_VERSION_CODE < KERNEL_VERSION(6,13,0)
		.vidioc_g_ctrl	   = hws_vidioc_g_ctrl,
		.vidioc_s_ctrl	   = hws_vidioc_s_ctrl,
		.vidioc_queryctrl  = hws_vidioc_queryctrl,
		.vidioc_g_parm 	   = hws_vidioc_g_parm,
		.vidioc_s_parm     = hws_vidioc_s_parm,
	#else
		.vidioc_g_ext_ctrls =  hws_v4l2_g_ext_ctrls,
		.vidioc_s_ext_ctrls =  hws_v4l2_s_ext_ctrls,
		.vidioc_query_ext_ctrl =  hws_v4l2_query_ext_ctrl,
		.vidioc_g_parm 	   = hws_vidioc_g_parm,
		.vidioc_s_parm     = hws_vidioc_s_parm,
	#endif

	.vidioc_enum_input = hws_vidioc_enum_input,
	.vidioc_g_input = hws_vidioc_g_input,
	.vidioc_s_input = hws_vidioc_s_input,
	.vidioc_log_status = hws_vidioc_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

};

static int hws_queue_setup(struct vb2_queue *q,
			   unsigned int *num_buffers, unsigned int *num_planes,
			   unsigned int sizes[], struct device *alloc_devs[])
{
	struct hws_video *videodev = q->drv_priv;
	struct hws_pcie_dev *pdx = videodev->dev;
	unsigned long flags;
	unsigned size;
	spin_lock_irqsave(&pdx->videoslock[videodev->index], flags);	
	size = 2* videodev->current_out_width * videodev->curren_out_height; // 16bit
	//printk( "%s(%d)->%d[%d?=%d]\n", __func__,videodev->index,videodev->fileindex,sizes[0],size);
	if(videodev->fileindex >1)
	{
		spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);
		return -EINVAL;
	}
	//printk( "q->num_buffers = %d *num_buffers =%d \n", q->num_buffers,*num_buffers);
	//if (tot_bufs < 2)
	//	tot_bufs = 2;
	//tot_bufs = hws_buffer_count(size, tot_bufs);
	//*num_buffers = tot_bufs - q->num_buffers;
	if (*num_planes)
	{
		if(sizes[0] < size)
		{
			spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);
			return -EINVAL;
		}
		else
		{
			spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);
			return  0;
		}
	}
	//printk( "%s()  num_buffers:%x tot_bufs:%x\n", __func__,*num_buffers,tot_bufs);
	//printk( "%s()  sizes[0]= %d size= %d\n", __func__,sizes[0],size);
	*num_planes = 1;
	sizes[0] = size; 
	spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);
	return 0;
}

static int hws_buffer_prepare(struct vb2_buffer *vb)
{

	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct hwsvideo_buffer *buf =
		container_of(vbuf, struct hwsvideo_buffer, vb);
	struct hws_video *videodev = vb->vb2_queue->drv_priv;
	struct hws_pcie_dev *pdx = videodev->dev;
	u32 size;
	unsigned long flags;
	//printk( "%s(W = %d H=%d)\n", __func__,videodev->current_out_width,videodev->curren_out_height);
	spin_lock_irqsave(&pdx->videoslock[videodev->index], flags);	
	size = 2* videodev->current_out_width * videodev->curren_out_height; // 16bit
	if (vb2_plane_size(vb, 0) < size)
	{
		spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);
		return -EINVAL;
	}
	vb2_set_plane_payload(vb, 0, size);
	buf->mem = vb2_plane_vaddr(vb,0);
	spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);
	return 0;	
}

static void hws_buffer_finish(struct vb2_buffer *vb)
{
	
	//struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	//struct hwsvideo_buffer *buf =
	//	container_of(vbuf, struct hwsvideo_buffer, vb);
	//struct hws_video *videodev = vb->vb2_queue->drv_priv;
	//printk( "%s()\n", __func__);
	return;
}

static void hws_buffer_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct hws_video *videodev = vb->vb2_queue->drv_priv;
	struct hwsvideo_buffer *buf =
		container_of(vbuf, struct hwsvideo_buffer, vb);
	unsigned long flags;
	struct hws_pcie_dev *pdx = videodev->dev;
	
	//printk( "%s(%d)\n", __func__,videodev->index);
	spin_lock_irqsave(&pdx->videoslock[videodev->index], flags);	
	list_add_tail(&buf->queue, &videodev->queue);
	spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);
}

static int hws_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct hws_video *videodev = q->drv_priv;
	unsigned long flags;
	struct hws_pcie_dev *pdx = videodev->dev;
	//printk( "%s(%d)->%d\n", __func__,videodev->index,videodev->fileindex);
	#if 0
	if(videodev->fileindex >1)
	{
		return -EINVAL;
	}
	#endif 
	videodev->seqnr = 0;
	WRITE_ONCE(videodev->next_frame_ts_ns, 0);
	videodev->output_rate_accum = 0;
	hws_diag_last_fresh_ns[videodev->index] = 0;
	hws_source_interval_ns_avg[videodev->index] = 0;
	msleep(100);
	//---------------
	//if(videodev->fileindex==1)
	//{
		//printk( "StartVideoCapture %s(%d)->%d\n", __func__,videodev->index,videodev->fileindex);
		StartVideoCapture(videodev->dev,videodev->index);
		videodev->startstreamIndex++;
		//------------------------ reset queue
   	   //printk( "%s(%d)->%d  reset queue \n", __func__,videodev->index,videodev->fileindex);
		
	//}
	spin_lock_irqsave(&pdx->videoslock[videodev->index], flags); 
		while (!list_empty(&videodev->queue)) {
			struct hwsvideo_buffer *buf = list_entry(videodev->queue.next,
				struct hwsvideo_buffer, queue);
			list_del(&buf->queue);
			
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		}
	spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);
	//-----------------------
	return 0;
}

static void hws_stop_streaming(struct vb2_queue *q)
{
	struct hws_video *videodev = q->drv_priv;
	unsigned long flags;
	struct hws_pcie_dev *pdx = videodev->dev;
	//printk( "%s(%d)->%d\n", __func__,videodev->index,videodev->fileindex);
	
	//if(videodev->seqnr){
		//vb2_wait_for_all_buffers(q);		
	//	mdelay(100);
		//printk( "%s() vb2_wait_for_all_buffers\n", __func__);
	//}
	#if 1
	//-----------------------------------
	WRITE_ONCE(videodev->next_frame_ts_ns, 0);
	videodev->output_rate_accum = 0;
	hws_diag_last_fresh_ns[videodev->index] = 0;
	hws_source_interval_ns_avg[videodev->index] = 0;
	videodev->startstreamIndex --;
	if(videodev->startstreamIndex<0) videodev->startstreamIndex=0;
	if(videodev->startstreamIndex == 0)
	{
		//printk( "StopVideoCapture %s(%d)->%d [%d]\n", __func__,videodev->index,videodev->fileindex,videodev->startstreamIndex);
		StopVideoCapture(videodev->dev,videodev->index);
	}
	//------------------
	spin_lock_irqsave(&pdx->videoslock[videodev->index], flags);	
	while (!list_empty(&videodev->queue)) {
		struct hwsvideo_buffer *buf = list_entry(videodev->queue.next,
			struct hwsvideo_buffer, queue);
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);
	//-----------------------------------------------------------------
	#endif 
	
}

static const struct vb2_ops hwspcie_video_qops = {
	.queue_setup    = hws_queue_setup,
	.buf_prepare  = hws_buffer_prepare,
	.buf_finish = hws_buffer_finish,
	.buf_queue    = hws_buffer_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.start_streaming = hws_start_streaming,
	.stop_streaming = hws_stop_streaming,
};

/*
 * B1 multi-consumer vb2 ops: q->drv_priv is struct hws_vfh_ctx
 */
static int hws_queue_setup_multi(struct vb2_queue *q,
               unsigned int *num_buffers, unsigned int *num_planes,
               unsigned int sizes[], struct device *alloc_devs[])
{
    struct hws_vfh_ctx *ctx = q->drv_priv;
    struct hws_video *videodev = ctx->video;
    struct hws_pcie_dev *pdx = videodev->dev;
    unsigned long flags;
    unsigned int size;

    spin_lock_irqsave(&pdx->videoslock[videodev->index], flags);
    size = 2 * videodev->current_out_width * videodev->curren_out_height;
    spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);

    if (*num_planes) {
        if (sizes[0] < size)
            return -EINVAL;
        return 0;
    }

    *num_planes = 1;
    sizes[0] = size;
    return 0;
}

static int hws_buffer_prepare_multi(struct vb2_buffer *vb)
{
    struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
    struct hwsvideo_buffer *buf = container_of(vbuf, struct hwsvideo_buffer, vb);
    struct hws_vfh_ctx *ctx = vb->vb2_queue->drv_priv;
    struct hws_video *videodev = ctx->video;
    struct hws_pcie_dev *pdx = videodev->dev;
    unsigned long flags;
    u32 size;

    spin_lock_irqsave(&pdx->videoslock[videodev->index], flags);
    size = 2 * videodev->current_out_width * videodev->curren_out_height;
    spin_unlock_irqrestore(&pdx->videoslock[videodev->index], flags);

    if (vb2_plane_size(vb, 0) < size)
        return -EINVAL;

    vb2_set_plane_payload(vb, 0, size);
    buf->mem = vb2_plane_vaddr(vb, 0);
    return 0;
}

static void hws_buffer_queue_multi(struct vb2_buffer *vb)
{
    struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
    struct hwsvideo_buffer *buf = container_of(vbuf, struct hwsvideo_buffer, vb);
    struct hws_vfh_ctx *ctx = vb->vb2_queue->drv_priv;
    unsigned long flags;

    spin_lock_irqsave(&ctx->qlock, flags);
    list_add_tail(&buf->queue, &ctx->buf_queue);
    spin_unlock_irqrestore(&ctx->qlock, flags);
}

static int hws_start_streaming_multi(struct vb2_queue *q, unsigned int count)
{
    struct hws_vfh_ctx *ctx = q->drv_priv;
    struct hws_video *videodev = ctx->video;

    ctx->streaming = true;
    ctx->seqnr = 0;
    WRITE_ONCE(videodev->next_frame_ts_ns, 0);
	videodev->output_rate_accum = 0;
    hws_diag_last_fresh_ns[videodev->index] = 0;
	hws_source_interval_ns_avg[videodev->index] = 0;

    /* Start hardware engine only once, on first streamer */
    if (atomic_inc_return(&videodev->engine_users) == 1) {
        videodev->seqnr = 0;
        StartVideoCapture(videodev->dev, videodev->index);
    }
    return 0;
}

static void hws_stop_streaming_multi(struct vb2_queue *q)
{
    struct hws_vfh_ctx *ctx = q->drv_priv;
    struct hws_video *videodev = ctx->video;
    struct hwsvideo_buffer *buf;
    unsigned long flags;

    ctx->streaming = false;
    WRITE_ONCE(videodev->next_frame_ts_ns, 0);
	videodev->output_rate_accum = 0;
    hws_diag_last_fresh_ns[videodev->index] = 0;
	hws_source_interval_ns_avg[videodev->index] = 0;

    /*
     * Serialize with worker-side dequeue so buffers from this ctx are not
     * completed after vb2 teardown starts.
     */
    flush_work(&videodev->videowork);

    /* Return all pending buffers for this ctx */
    spin_lock_irqsave(&ctx->qlock, flags);
    while (!list_empty(&ctx->buf_queue)) {
        buf = list_first_entry(&ctx->buf_queue, struct hwsvideo_buffer, queue);
        list_del(&buf->queue);
        vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
    }
    spin_unlock_irqrestore(&ctx->qlock, flags);

    /* Stop engine when the last streamer goes away */
    if (atomic_dec_return(&videodev->engine_users) == 0)
        StopVideoCapture(videodev->dev, videodev->index);
}

static const struct vb2_ops hwspcie_video_multi_qops = {
    .queue_setup    = hws_queue_setup_multi,
    .buf_prepare    = hws_buffer_prepare_multi,
    .buf_finish     = hws_buffer_finish,
    .buf_queue      = hws_buffer_queue_multi,
    .wait_prepare   = vb2_ops_wait_prepare,
    .wait_finish    = vb2_ops_wait_finish,
    .start_streaming= hws_start_streaming_multi,
    .stop_streaming = hws_stop_streaming_multi,
};
//-----------------------------------------
const unsigned char  g_YUVColors [MAX_COLOR][3] = {
    {128, 16, 128},     // BLACK
    {128, 235 , 128},    // WHITE
    {16, 211, 146},     // YELLOW
    {166, 170, 16},     // CYAN
    {54, 145, 34},      // GREEN
    {202, 106, 222},    // MAGENTA
    {90, 81, 240},      // RED
    {240, 41, 109},     // BLUE
    {128, 125, 128},    // GREY
};
	

static void SetNoVideoMem(uint8_t * pDest, int  w,int h)
{
int x,y;
uint8_t *pST;
uint8_t *pNS;
pST = (uint8_t *)pDest;
//printk("SetNoVideoMem[%d-%d]\n",w,h);

for(x=0;x<w/2;x++)
{
	pST[0] = 41;
	pST[1] = 240;
	pST[2] = 41;
	pST[3] = 109;
	pST +=4;
}

pNS = pDest+w*2;
for(y=1;y<h;y++)
{
	memcpy(pNS,pDest,w*2);
	pNS = pNS+w*2;
}
}
//---------------------------------------------------
static int _deliver_samples(struct hws_audio *drv, void *aud_data, u32 aud_len)
{
	struct snd_pcm_substream *substream = READ_ONCE(drv->substream);
	struct snd_pcm_runtime *runtime;
	int ch = drv->index;
	unsigned long flags;
	u8 *src;
	u8 *dst;
	unsigned int frame_bytes;
	unsigned int frames;
	unsigned int ring_size;
	unsigned int wpos;
	unsigned int period_size;
	unsigned int period_used;
	unsigned int wrapped_frames = 0;
	bool diag = hws_diag_enabled();
	int elapsed = 0;
	u64 now_ns = ktime_get_ns();
	u64 last_irq_ns = READ_ONCE(drv->last_irq_ns);
	u64 last_copy_ns = READ_ONCE(drv->last_copy_ns);
	u64 packet_ns = 0;
	u64 max_latency_window_ns = 0;

	if (ch < 0 || ch >= MAX_VID_CHANNELS)
		diag = false;

	if (!substream) {
		if (diag)
			atomic64_inc(&hws_audio_diag[ch].dropped_no_substream);
		return -ENODEV;
	}

	runtime = substream->runtime;
	if (!runtime || !runtime->dma_area || drv->channels <= 0) {
		if (diag)
			atomic64_inc(&hws_audio_diag[ch].dropped_bad_runtime);
		return -ENODEV;
	}

	frame_bytes = 2 * drv->channels;
	if (frame_bytes == 0)
		return -EINVAL;

	if (aud_len < frame_bytes || (aud_len % frame_bytes) != 0) {
		if (diag)
			atomic64_inc(&hws_audio_diag[ch].bad_packet_sizes);
	}

	frames = aud_len / frame_bytes;
	if (frames == 0)
		return 0;
	packet_ns = hws_audio_packet_duration_ns(drv, aud_len);
	if (packet_ns)
		max_latency_window_ns = packet_ns * 4;

	src = (u8 *)aud_data;
	dst = (u8 *)runtime->dma_area;

	spin_lock_irqsave(&drv->ring_lock, flags);
	ring_size = drv->ring_size_byframes;
	wpos = drv->ring_wpos_byframes;
	period_size = drv->period_size_byframes;
	period_used = drv->period_used_byframes;
	spin_unlock_irqrestore(&drv->ring_lock, flags);

	if (ring_size == 0 || period_size == 0) {
		if (diag)
			atomic64_inc(&hws_audio_diag[ch].dropped_ring_not_ready);
		return -EAGAIN;
	}

	/* Keep newest data if a single packet exceeds ring capacity. */
	if (frames > ring_size) {
		unsigned int drop = frames - ring_size;
		if (diag)
			atomic64_inc(&hws_audio_diag[ch].oversized_packets);
		src += drop * frame_bytes;
		frames = ring_size;
		aud_len = frames * frame_bytes;
	}

	if (wpos + frames > ring_size) {
		wrapped_frames = ring_size - wpos;
		memcpy(dst + wpos * frame_bytes, src, wrapped_frames * frame_bytes);
		memcpy(dst, src + wrapped_frames * frame_bytes, aud_len - wrapped_frames * frame_bytes);
	} else {
		memcpy(dst + wpos * frame_bytes, src, aud_len);
	}

	spin_lock_irqsave(&drv->ring_lock, flags);
	drv->ring_wpos_byframes = (wpos + frames) % ring_size;
	elapsed = (period_used + frames) / period_size;
	drv->period_used_byframes = (period_used + frames) % period_size;
	spin_unlock_irqrestore(&drv->ring_lock, flags);

	if (elapsed && READ_ONCE(drv->substream) == substream) {
		unsigned int i;

		/*
		 * A single DMA packet can cover multiple ALSA periods. Report each
		 * elapsed period so low-quantum userspace does not undercount wakeups.
		 */
		for (i = 0; i < elapsed; i++)
			snd_pcm_period_elapsed(substream);
		if (diag)
			atomic64_add(elapsed, &hws_audio_diag[ch].period_elapsed_calls);
	}

	if (diag) {
		atomic64_add(frames * frame_bytes, &hws_audio_diag[ch].delivered_bytes);
		atomic64_add(frames, &hws_audio_diag[ch].delivered_frames);
		if (last_copy_ns && now_ns >= last_copy_ns &&
		    (!max_latency_window_ns || now_ns - last_copy_ns <= max_latency_window_ns))
			hws_audio_diag_record_latency(ch, &hws_audio_diag[ch].copy_to_deliver_ns_total,
						 &hws_audio_diag[ch].copy_to_deliver_ns_max,
						 &hws_audio_diag[ch].copy_to_deliver_samples,
						 now_ns - last_copy_ns);
		if (last_irq_ns && now_ns >= last_irq_ns &&
		    (!max_latency_window_ns || now_ns - last_irq_ns <= max_latency_window_ns))
			hws_audio_diag_record_latency(ch, &hws_audio_diag[ch].irq_to_deliver_ns_total,
						 &hws_audio_diag[ch].irq_to_deliver_ns_max,
						 &hws_audio_diag[ch].irq_to_deliver_samples,
						 now_ns - last_irq_ns);
	}
	WRITE_ONCE(drv->last_progress_ns, now_ns);

	return frames * frame_bytes;
}


static const char *hws_audio_silence_reason_name(enum hws_audio_silence_reason reason)
{
	switch (reason) {
	case HWS_AUDIO_SILENCE_NO_VIDEO:
		return "no_video";
	case HWS_AUDIO_SILENCE_TIMER:
		return "timer";
	case HWS_AUDIO_SILENCE_FALLBACK:
	default:
		return "fallback";
	}
}

static void hws_inject_silence_packet(struct hws_pcie_dev *pdx, int dwAudioCh,
			      unsigned int packet_bytes, enum hws_audio_silence_reason reason)
{
	unsigned int frame_bytes;
	unsigned int remain;
	static const u8 silence_chunk[512] = { 0 };

	if (dwAudioCh < 0 || dwAudioCh >= MAX_VID_CHANNELS)
		return;

	frame_bytes = (pdx->audio[dwAudioCh].channels > 0) ?
		(2U * (unsigned int)pdx->audio[dwAudioCh].channels) : 0U;
	if (frame_bytes == 0U)
		return;

	remain = packet_bytes;
	remain -= remain % frame_bytes;
	while (remain > 0U) {
		unsigned int chunk = remain;
		int delivered;

		if (chunk > sizeof(silence_chunk))
			chunk = sizeof(silence_chunk);
		chunk -= chunk % frame_bytes;
		if (chunk == 0U)
			break;

		delivered = _deliver_samples(&pdx->audio[dwAudioCh], (void *)silence_chunk, chunk);
		if (delivered < 0) {
			if (hws_diag_enabled())
				atomic64_inc(&hws_audio_diag[dwAudioCh].delivery_errors);
			break;
		}
		remain -= chunk;
	}

	if (hws_diag_enabled()) {
		if (reason == HWS_AUDIO_SILENCE_NO_VIDEO)
			atomic64_inc(&hws_audio_diag[dwAudioCh].no_video_silence_injects);
		else if (reason == HWS_AUDIO_SILENCE_TIMER)
			atomic64_inc(&hws_audio_diag[dwAudioCh].timer_silence_injects);
		else
			atomic64_inc(&hws_audio_diag[dwAudioCh].fallback_silence_injects);
	}
	if (hws_audio_trace_enabled())
		trace_printk("hws_audio_silence ch=%d bytes=%u reason=%s\n",
			     dwAudioCh, packet_bytes, hws_audio_silence_reason_name(reason));
}

static void hws_audio_silence_fallback_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct hws_audio *drv = container_of(dwork, struct hws_audio, silence_work);
	struct hws_pcie_dev *pdx = drv->dev;
	int ch = drv->index;
	bool diag = hws_diag_enabled();
	u32 packet_bytes;
	u64 packet_ns;
	u64 now_ns;
	u64 last_progress_ns;
	u64 last_irq_ns;

	if (diag && ch >= 0 && ch < MAX_VID_CHANNELS)
		atomic64_inc(&hws_audio_diag[ch].timer_runs);

	if (!hws_audio_stream_active(drv))
		return;

	packet_bytes = hws_audio_effective_packet_bytes(drv, 0);
	packet_ns = hws_audio_packet_duration_ns(drv, packet_bytes);
	if (!packet_bytes || !packet_ns)
		goto reschedule;

	now_ns = ktime_get_ns();
	last_progress_ns = READ_ONCE(drv->last_progress_ns);
	last_irq_ns = READ_ONCE(drv->last_irq_ns);

	if (!last_progress_ns) {
		WRITE_ONCE(drv->last_progress_ns, now_ns);
		goto reschedule;
	}

	if (now_ns > last_progress_ns && now_ns - last_progress_ns >= packet_ns) {
		if (!last_irq_ns || now_ns - last_irq_ns >= packet_ns) {
			WRITE_ONCE(drv->last_copy_ns, 0);
			hws_inject_silence_packet(pdx, ch, packet_bytes, HWS_AUDIO_SILENCE_TIMER);
		}
	}

reschedule:
	if (hws_audio_stream_active(drv))
		queue_delayed_work(pdx->auwq, &drv->silence_work,
				   hws_audio_fallback_delay_jiffies(drv, packet_bytes));
}

static void audio_data_process(struct work_struct *p_work)
{
	struct hws_audio *drv = container_of(p_work, struct hws_audio, audiowork);
	struct hws_pcie_dev *pdx = drv->dev;
	int dwAudioCh;
	unsigned int budget;
	unsigned int processed = 0;
	unsigned long flags;
	bool diag = hws_diag_enabled();

	dwAudioCh = drv->index;
	if (dwAudioCh < 0 || dwAudioCh >= MAX_VID_CHANNELS)
		diag = false;
	if (diag)
		atomic64_inc(&hws_audio_diag[dwAudioCh].work_runs);

	if (!READ_ONCE(pdx->m_bAudioRun[dwAudioCh]))
		return;

	budget = (audio_work_budget > 0) ? (unsigned int)audio_work_budget : 1U;
	while (processed < budget) {
		int nIndex = -1;
		int i;
		int delivered = 0;
		BYTE *bBuf = NULL;
		int aud_len = 0;

		spin_lock_irqsave(&pdx->audiolock[dwAudioCh], flags);
		for (i = pdx->m_nRDAudioIndex[dwAudioCh]; i < MAX_AUDIO_QUEUE; i++) {
			if (pdx->m_AudioInfo[dwAudioCh].pStatusInfo[i].byLock == MEM_LOCK) {
				nIndex = i;
				bBuf = pdx->m_AudioInfo[dwAudioCh].m_pAudioBufData[i];
				aud_len = pdx->m_AudioInfo[dwAudioCh].pStatusInfo[i].dwLength;
				break;
			}
		}

		if (nIndex == -1) {
			for (i = 0; i < pdx->m_nRDAudioIndex[dwAudioCh]; i++) {
				if (pdx->m_AudioInfo[dwAudioCh].pStatusInfo[i].byLock == MEM_LOCK) {
					nIndex = i;
					bBuf = pdx->m_AudioInfo[dwAudioCh].m_pAudioBufData[i];
					aud_len = pdx->m_AudioInfo[dwAudioCh].pStatusInfo[i].dwLength;
					break;
				}
			}
		}

		if (nIndex != -1) {
			if (diag)
				atomic64_inc(&hws_audio_diag[dwAudioCh].buffers_found);
			pdx->m_nRDAudioIndex[dwAudioCh] = nIndex + 1;
			if (pdx->m_nRDAudioIndex[dwAudioCh] >= MAX_AUDIO_QUEUE)
				pdx->m_nRDAudioIndex[dwAudioCh] = 0;
		}
		spin_unlock_irqrestore(&pdx->audiolock[dwAudioCh], flags);

		if (nIndex == -1 || !bBuf)
			break;

		if (!READ_ONCE(pdx->m_bAudioRun[dwAudioCh])) {
			spin_lock_irqsave(&pdx->audiolock[dwAudioCh], flags);
			pdx->m_AudioInfo[dwAudioCh].pStatusInfo[nIndex].byLock = MEM_UNLOCK;
			spin_unlock_irqrestore(&pdx->audiolock[dwAudioCh], flags);
			break;
		}

		/* Do expensive copy/callback path outside device audio lock. */
		delivered = _deliver_samples(drv, bBuf, aud_len);
		if (delivered < 0) {
			if (diag) {
				atomic64_inc(&hws_audio_diag[dwAudioCh].delivery_errors);
				pr_info_ratelimited("hws: audio delivery skipped ch=%d err=%d\n", dwAudioCh, delivered);
			}
		}

		spin_lock_irqsave(&pdx->audiolock[dwAudioCh], flags);
		pdx->m_AudioInfo[dwAudioCh].pStatusInfo[nIndex].byLock = MEM_UNLOCK;
		spin_unlock_irqrestore(&pdx->audiolock[dwAudioCh], flags);
		processed++;
	}
}

//-------------------------------
static void FHD_To_800X600_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
//   BYTE*pDestBuf;
   int *pSrcData;
   int *pDestData;
   pSrcBuf = pSrc;
   pDestData = (int *) pOut;
   for(y=0;y<in_h;y++)
   {
	  switch(y%9)
	  {
		case 0:
		case 2:
		case 4:
		case 6:
	    case 8:
		{
		  pSrcBuf = pSrc + (y*in_w*2)+(240*2);
		  for(x=0;x<(in_w-480);)
		  {
			 pSrcData =(int *)pSrcBuf;
			pDestData[0] = pSrcData[0];
			pDestData[1] = pSrcData[2];
			pDestData[2] = pSrcData[4];
			pDestData[3] = pSrcData[6];
			pDestData[4] = pSrcData[8];
			 pSrcBuf +=18*2;
			 pDestData +=5;
			 x= x+18;
		  }
		  break;
	  }
	  case 1:
	  case 3:
	  case 5:
	  case 7:
	  {
			break;
	  }
	  }

   }

}
static void HD_To_800X600_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   //int *pSrcData;
//   int *pDestData;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   for(y=0;y<(in_h-120);y++)
   {
		memcpy(pDestBuf,(pSrcBuf+60*in_w*2),out_w*2);
		pDestBuf += out_w*2;
		pSrcBuf += in_w*2;
   }

}
static void SD_NTSC_To_SD_PAL_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   //int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   //int *pSrcData;
   //int *pDestData;
   DWORD dwI;
   DWORD dwJ;
   BYTE *pDstYUV;
   
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   
  
   // PAL 720X576  NTSC 720X480
   pDstYUV =  pDestBuf;	
   for(dwI =0; dwI <48; dwI++)
   {
		for(dwJ=0; dwJ<out_w; dwJ++)
		{
				pDstYUV[0] = 0x10;
				pDstYUV[1] = 0x80;
				pDstYUV +=2;
		}
	}
   pDestBuf += 48*in_w*2;
   memcpy(pDestBuf,pSrcBuf,in_h*in_w*2);
   pDestBuf +=in_h*in_w*2; 
   pDstYUV = pDestBuf;
  for(dwI =0; dwI <48; dwI++)
   {
		for(dwJ=0; dwJ<out_w; dwJ++)
		{
				pDstYUV[0] = 0x10;
				pDstYUV[1] = 0x80;
				pDstYUV +=2;
		}
	} 

}

static void SD_PAL_To_SD_NTSC_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
  // int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
//   int *pSrcData;
   //int *pDestData;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   // PAL 720X576  NTSC 720X480
   memcpy(pDestBuf,(pSrcBuf+48*in_w*2),out_h*out_w*2);
   
}
static void SD_NTSC_To_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   int dwJ;
   BYTE*pDstYUV;
   
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   
   
   // PAL 720X480   x*2 =1440,(y-36)*2= 1080
  
   //--- 60 line 
   for(y=0;y<60;y++)
   {
		pDstYUV = pDestBuf;
		for(dwJ=0; dwJ<out_w; dwJ++)
		{
				pDstYUV[0] = 0x10;
				pDstYUV[1] = 0x80;
				pDstYUV +=2;
		}
		pDestBuf +=out_w*2;
   }
   //---
    for(y=0;y<(out_h-120);)
   {
		pDstYUV = pDestBuf;
		for(dwJ=0; dwJ<240; dwJ++)
		{
				pDstYUV[0] = 0x10;
				pDstYUV[1] = 0x80;
				pDstYUV +=2;
		}
		pDestBuf +=240*2;
		for(x=0;x<720;)
		{

				pDestBuf[0] = pSrcBuf[0];//y
				pDestBuf[1] = pSrcBuf[1];//cb
						
				pDestBuf[2] = pSrcBuf[0];//y
				pDestBuf[3] = pSrcBuf[3];//cr

				pDestBuf[4] = pSrcBuf[2];//y
				pDestBuf[5] = pSrcBuf[1];//cb
						
				pDestBuf[6] = pSrcBuf[2];//y
				pDestBuf[7] = pSrcBuf[3];//cr

				pSrcBuf +=4;
				pDestBuf +=8;
				x= x+2;
		}
		pDstYUV = pDestBuf;
		for(dwJ=0; dwJ<240; dwJ++)
		{
				pDstYUV[0] = 0x10;
				pDstYUV[1] = 0x80;
				pDstYUV +=2;
		}
		pDestBuf +=240*2;
		//------------copy line 
		memcpy(pDestBuf,pDestBuf-(out_w*2),out_w*2);
		pDestBuf += out_w*2;
		y = y+2;
		//------------------
   }
   //-- 60 line 
   	for(y=0;y<60;y++)
  	 {
		pDstYUV = pDestBuf;
		for(dwJ=0; dwJ<out_w; dwJ++)
		{
				pDstYUV[0] = 0x10;
				pDstYUV[1] = 0x80;
				pDstYUV +=2;
		}
		pDestBuf +=out_w*2;
		
 	}
	//---
}

static void SD_PAL_To_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   int dwJ;
   BYTE*pDstYUV;
      pSrcBuf = pSrc;
   pDestBuf = pOut;
   // PAL 720X576   x*2 =1440,(y-36)*2= 1080
   pSrcBuf = pSrcBuf+18*in_w*2;
   for(y=0;y<out_h;)
   {
		pDstYUV = pDestBuf;
		for(dwJ=0; dwJ<240; dwJ++)
		{
				pDstYUV[0] = 0x10;
				pDstYUV[1] = 0x80;
				pDstYUV +=2;
		}
		pDestBuf +=240*2;
		for(x=0;x<720;)
		{

				pDestBuf[0] = pSrcBuf[0];//y
				pDestBuf[1] = pSrcBuf[1];//cb
						
				pDestBuf[2] = pSrcBuf[0];//y
				pDestBuf[3] = pSrcBuf[3];//cr

				pDestBuf[4] = pSrcBuf[2];//y
				pDestBuf[5] = pSrcBuf[1];//cb
						
				pDestBuf[6] = pSrcBuf[2];//y
				pDestBuf[7] = pSrcBuf[3];//cr

				pSrcBuf +=4;
				pDestBuf +=8;
				x= x+2;
		}
		pDstYUV = pDestBuf;
		for(dwJ=0; dwJ<240; dwJ++)
		{
				pDstYUV[0] = 0x10;
				pDstYUV[1] = 0x80;
				pDstYUV +=2;
		}
		pDestBuf +=240*2;
		//------------copy line 
		memcpy(pDestBuf,pDestBuf-(out_w*2),out_w*2);
		pDestBuf += out_w*2;
		y = y+2;
		//------------------
   }
   
   
}

static void SD_PAL_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
  int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
  // int dwJ;
//   BYTE*pDstYUV;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
 
   
   // PAL 720X576   640X360-> 1280*720
   pSrcBuf = pSrcBuf+108*in_w*2;
    for(y=0;y<out_h;)
   {
		pSrcBuf += 40*2;
		for(x=0;x<640;)
		{

				pDestBuf[0] = pSrcBuf[0];//y
				pDestBuf[1] = pSrcBuf[1];//cb
						
				pDestBuf[2] = pSrcBuf[0];//y
				pDestBuf[3] = pSrcBuf[3];//cr

				pDestBuf[4] = pSrcBuf[2];//y
				pDestBuf[5] = pSrcBuf[1];//cb
						
				pDestBuf[6] = pSrcBuf[2];//y
				pDestBuf[7] = pSrcBuf[3];//cr

				pSrcBuf +=4;
				pDestBuf +=8;
				x= x+2;
		}
		pSrcBuf += 40*2;
		//------------copy line 
		memcpy(pDestBuf,pDestBuf-(out_w*2),out_w*2);
		pDestBuf += out_w*2;
		y= y+2;
		//------------------
   }
   
   
}
static void V1280X1024_To_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{

		BYTE*pSrcBuf;
		BYTE*pDestBuf;
		pSrcBuf = pSrc;
		pDestBuf = pOut;
		pSrcBuf = pSrcBuf+152*in_w*2;
		HD_To_FHD_Scaler(pSrcBuf,pDestBuf,1280,720,out_w,out_h);
}
void V1280X1024_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
		BYTE*pSrcBuf;
		BYTE*pDestBuf;
		pSrcBuf = pSrc;
		pDestBuf = pOut;
		pSrcBuf = pSrcBuf+152*in_w*2;
		memcpy(pDestBuf,pSrcBuf,out_h*out_w*2);
			
}
void V1280X1024_To_800X600_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
		BYTE*pSrcBuf;
		BYTE*pDestBuf;
		pSrcBuf = pSrc;
		pDestBuf = pOut;
		pSrcBuf = pSrcBuf+152*in_w*2;
		HD_To_800X600_Scaler(pSrcBuf,pDestBuf,1280,720,out_w,out_h);


}

void SD_NTSC_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
//   int dwJ;
   //BYTE*pDstYUV;
   // PAL 720X480   640X360-> 1280*720
   pSrcBuf = pSrcBuf+60*in_w*2;
    for(y=0;y<out_h;)
   {
		pSrcBuf += 40*2;
		for(x=0;x<640;)
		{

				pDestBuf[0] = pSrcBuf[0];//y
				pDestBuf[1] = pSrcBuf[1];//cb
						
				pDestBuf[2] = pSrcBuf[0];//y
				pDestBuf[3] = pSrcBuf[3];//cr

				pDestBuf[4] = pSrcBuf[2];//y
				pDestBuf[5] = pSrcBuf[1];//cb
						
				pDestBuf[6] = pSrcBuf[2];//y
				pDestBuf[7] = pSrcBuf[3];//cr

				pSrcBuf +=4;
				pDestBuf +=8;
				x= x+2;
		}
		pSrcBuf += 40*2;
		//------------copy line 
		memcpy(pDestBuf,pDestBuf-(out_w*2),out_w*2);
		pDestBuf += out_w*2;
		y= y+2;
		//------------------
   }
   
   
}

void FHD_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)

{
   int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   int *pSrcData;
   int *pDestData;
   pSrcBuf = pSrc;
   pDestData = (int *) pOut;
   for(y=0;y<in_h;y++)
   {
	  if(y%3 != 2)
	  {
		  for(x=0;x<in_w;)
		  {
			pSrcData =(int *)pSrcBuf;
			*pDestData = *pSrcData;
			 if(x%2==1)
			 {
				pDestBuf =(BYTE *)pDestData;
				pDestBuf[1] = pSrcBuf[3];
				pDestBuf [3] = pSrcBuf[1];
			 }
			pDestData +=1;
			pSrcBuf +=6;
			x= x+3;
		  }
	  }
	  else
	  {
		pSrcBuf += in_w*2;	
	  }

   }

}
void HD_To_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
	   
	   	 int x,y;
		 BYTE*pSrcBuf;
		 //BYTE*pDestBuf;
		 DWORD *pSrcData;
		 DWORD *pDestData;
		 int out_w_size;
		 //BYTE*pSrcTmp;
		 BYTE*pDestTmp;
		 pDestData = (DWORD *) pOut;
		 pSrcData = (DWORD *) pSrc;
		 out_w_size = out_w*2;
		 for(y=0;y<out_h;y++)
		 {
			
				if((y%3)==2)
				{
					pSrcBuf = (BYTE*)pDestData;
					memcpy(pSrcBuf,(BYTE*)(pSrcBuf-out_w_size),out_w_size);
					pDestData += out_w_size/4;
				}
				else
				{
				
					for(x=0;x<out_w;)
					{
						*pDestData = *pSrcData;
						 pDestData ++;
						 pSrcData++;
						 
						*pDestData = *pSrcData;
						 pDestTmp =(BYTE*)pDestData;
						 pDestTmp[2] = pDestTmp[0];
						 pDestData ++;
						
						*pDestData = *pSrcData;
						 pDestTmp =(BYTE*)pDestData;
						 pDestTmp[0] = pDestTmp[2];
						 pSrcData++;
						 pDestData++;
						 x= x+6;
					}
			
				}
				
	   
		 }
	   

}
void FHD_To_SD_NTSC_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   BYTE *pSrcData;
//   BYTE *pDestData;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   //-- 1920X1080 0->720X480
   pSrcBuf = pSrcBuf + 60*in_w*2;
   for(y=0;y<out_h;)
   {
		pSrcData = pSrcBuf + 240*2;
		for(x =0;x<720;)
		{
			pDestBuf[0] = pSrcData[0];//y
			pDestBuf[1] = pSrcData[1];//cb

			pDestBuf[2] = pSrcData[4];//y
			pDestBuf[3] = pSrcData[3];//cr

			x = x+ 2;
			pSrcData +=8;
			pDestBuf +=4;
		}
		pSrcBuf += in_w*4;
		y= y+1;
   }
   
}
void FHD_To_SD_PAL_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   BYTE *pSrcData;
   BYTE*pDstYUV;
//   BYTE *pDestData;
   int dwJ;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   
   //-- 1920X1080 0->720X576
   //-------------------
   pDstYUV = pDestBuf;
   for(dwJ=0; dwJ<18*out_w; dwJ++)
	{
			pDstYUV[0] = 0x10;
			pDstYUV[1] = 0x80;
			pDstYUV +=2;
	}
   //------------------
   pDestBuf += 18*out_w*2;
    for(y=0;y<540;)
   {
		pSrcData = pSrcBuf + 240*2;
		for(x =0;x<720;)
		{
			pDestBuf[0] = pSrcData[0];//y
			pDestBuf[1] = pSrcData[1];//cb

			pDestBuf[2] = pSrcData[4];//y
			pDestBuf[3] = pSrcData[3];//cr

			x = x+ 2;
			pSrcData +=8;
			pDestBuf +=4;
		}
		pSrcBuf += in_w*4;
		y= y+1;
   }
   //------------
    pDstYUV = pDestBuf;
   for(dwJ=0; dwJ<18*out_w; dwJ++)
	{
			pDstYUV[0] = 0x10;
			pDstYUV[1] = 0x80;
			pDstYUV +=2;
	}
   //------------
}

void HD_To_SD_NTSC_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   //int *pSrcData;
  // int *pDestData;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   //-- 1280X720 ->720X480
   pSrcBuf = pSrcBuf + 120*in_w*2;
   for(y=0;y<out_h;)
   {
		memcpy(pDestBuf,pSrcBuf+280*2,out_w*2);
		pSrcBuf += in_w*2;
		pDestBuf += out_w*2;
		y= y+1;
   }
   
}
void HD_To_SD_PAL_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   //int dwJ;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   //BYTE*pDstYUV;
   //-- 1920X1080 0->720X576
   pSrcBuf = pSrcBuf + 72*in_w*2;
   for(y=0;y<out_h;)
   {
		memcpy(pDestBuf,pSrcBuf+280*2,out_w*2);
		pSrcBuf += in_w*2;
		pDestBuf += out_w*2;
		y= y+1;
   }
}
void V1280X1024_NTSC_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
//   BYTE*pDstYUV;
   //-- 1280X1024 0->720X480
   pSrcBuf = pSrcBuf + 272*in_w*2; 
   for(y=0;y<out_h;)
   {
		memcpy(pDestBuf,pSrcBuf+280*2,out_w*2);
		pSrcBuf += in_w*2;
		pDestBuf += out_w*2;
		y= y+1;
   }
}

void V1280X1024_PAL_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
//   BYTE*pDstYUV;
   //-- 1280x1024 0->720X576
   pSrcBuf = pSrcBuf + 224*in_w*2;
   for(y=0;y<out_h;)
   {
		memcpy(pDestBuf,pSrcBuf+280*2,out_w*2);
		pSrcBuf += in_w*2;
		pDestBuf += out_w*2;
		y= y+1;
   }
}
void All_VideoScaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   BYTE *pDstYUV;
   int  missX=0;
   int  missY=0;
   int  dumyX=0;
   int  dumyY=0;
   int dwJ; 
   int  y;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   if(in_w>out_w)
   {
		missX = (in_w-out_w)/2;
   }
   else
   {
   		dumyX = (out_w- in_w)/2;

   }
   if(in_h>out_h)
   {
		missY = (in_h-out_h)/2;
   }
   else
   	{
		dumyY = (out_h - in_h)/2;
   }
   if(dumyY>0)
   {
    pDstYUV = pDestBuf;
	for(dwJ=0; dwJ<(dumyY*out_w); dwJ++)
	{
			pDstYUV[0] = 0x10;
			pDstYUV[1] = 0x80;
			pDstYUV +=2;
	}
	pDestBuf = pDestBuf+dumyY*out_w*2;
   }
   if(missY>0)
   {
	  pSrcBuf += missY*in_w*2;
   }
   //----------
   
   for(y=0;y<(out_h-(dumyY*2));y++)
   {
		if ((y & 0x1f) == 0)
			cond_resched();
   		if(dumyX>0)
   		{
   			pDstYUV = pDestBuf;
			for(dwJ=0; dwJ<dumyX; dwJ++)
			{
				pDstYUV[0] = 0x10;
				pDstYUV[1] = 0x80;
				pDstYUV +=2;
			}
			pDestBuf += dumyX*2;
		}
		memcpy(pDestBuf,pSrcBuf+missX*2,(out_w-(dumyX*2))*2);
		pSrcBuf += in_w*2;
		pDestBuf += (out_w-(dumyX*2))*2;
		if(dumyX>0)
   		{
			pDstYUV = pDestBuf;
			for(dwJ=0; dwJ<dumyX; dwJ++)
			{
				pDstYUV[0] = 0x10;
				pDstYUV[1] = 0x80;
				pDstYUV +=2;
			}
			pDestBuf += dumyX*2;
		}
   }
   if(dumyY>0)
  {
  	pDstYUV = pDestBuf;
	for(dwJ=0; dwJ<(dumyY*out_w); dwJ++)
	{
			pDstYUV[0] = 0x10;
			pDstYUV[1] = 0x80;
			pDstYUV +=2;
	}

  }

}

static void UHD_To_800X600_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)

{
   int x,y;
   BYTE*pSrcBuf;
//   BYTE*pDestBuf;
   BYTE *pSrcData;
   BYTE *pDestData;
   pSrcBuf = pSrc;
   pDestData = (BYTE *) pOut;
   for(y=0;y<in_h;y++)
   {
	  switch(y%18)
	  {
		case 0:
		case 4:
		case 8:
		case 12:
	    case 16:
		{
		  pSrcBuf = pSrc + (y*in_w*2)+(480*2);
		  for(x=0;x<(in_w-960);)
		  {
			 pSrcData =(BYTE *)pSrcBuf;
			 
			 pDestData[0] = pSrcData[0];
			 pDestData[1] = pSrcData[1];
			 pDestData[2] = pSrcData[8];
			 pDestData[3] = pSrcData[7];

			 pDestData[4] = pSrcData[16];
			 pDestData[5] = pSrcData[17];
			 pDestData[6] = pSrcData[24];
			 pDestData[7] = pSrcData[23];

			 pDestData[8] = pSrcData[32];
			 pDestData[9] = pSrcData[33];
			 pDestData[10] = pSrcData[36];
			 pDestData[11] = pSrcData[35];

			 pDestData[12] = pSrcData[44];
			 pDestData[13] = pSrcData[45];
			 pDestData[14] = pSrcData[52];
			 pDestData[15] = pSrcData[51];

			 pDestData[16] = pSrcData[60];
			 pDestData[17] = pSrcData[61];
			 pDestData[18] = pSrcData[68];
			 pDestData[19] = pSrcData[67];
			 
			 
			 pSrcBuf +=18*4;
			 pDestData +=5*4;
			 x= x+18*2;
		  }
		  break;
	  }
	  default:
	  {
			break;
	  }
	  }

   }

}
static void UHDW_To_800X600_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)

{
   int x,y;
   BYTE*pSrcBuf;
   //BYTE*pDestBuf;
   BYTE *pSrcData;
   BYTE *pDestData;
   pSrcBuf = pSrc;
   pDestData = (BYTE *) pOut;
   for(y=0;y<in_h;y++)
   {

	  switch(y%18)
	  {
		case 0:
		case 4:
		case 8:
		case 12:
	    case 16:
		{
		  pSrcBuf = pSrc + (y*in_w*2)+((480+128)*2);
		  for(x=0;x<(3840-960);)
		  {
			 pSrcData =(BYTE *)pSrcBuf;
			 
			 pDestData[0] = pSrcData[0];
			 pDestData[1] = pSrcData[1];
			 pDestData[2] = pSrcData[8];
			 pDestData[3] = pSrcData[7];

			 pDestData[4] = pSrcData[16];
			 pDestData[5] = pSrcData[17];
			 pDestData[6] = pSrcData[24];
			 pDestData[7] = pSrcData[23];

			 pDestData[8] = pSrcData[32];
			 pDestData[9] = pSrcData[33];
			 pDestData[10] = pSrcData[36];
			 pDestData[11] = pSrcData[35];

			 pDestData[12] = pSrcData[44];
			 pDestData[13] = pSrcData[45];
			 pDestData[14] = pSrcData[52];
			 pDestData[15] = pSrcData[51];

			 pDestData[16] = pSrcData[60];
			 pDestData[17] = pSrcData[61];
			 pDestData[18] = pSrcData[68];
			 pDestData[19] = pSrcData[67];
			 
			 
			 pSrcBuf +=18*4;
			 pDestData +=5*4;
			 x= x+18*2;
		  }
		  break;
	  }
	  default:
	  {
			break;
	  }
	  }

   }

}

static void V2560X1440_To_800X600_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)

{
   int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   BYTE*pSrcData;
   //BYTE*pDstYUV;
	
   pSrcBuf = pSrc;
   pDestBuf = pOut;
  
   //-- V2560X1440->1280X720
   for(y=0;y<out_h;y++)
   {
		pSrcData = pSrcBuf + 480*2;
		for(x =0;x<out_w;)
		{
			pDestBuf[0] = pSrcData[0];//y
			pDestBuf[1] = pSrcData[1];//cb

			pDestBuf[2] = pSrcData[4];//y
			pDestBuf[3] = pSrcData[3];//cr

			x = x+ 2;
			pSrcData += 8;
			pDestBuf +=4;
		}
		
		pSrcBuf += in_w*4;
   }

}

static  void UHD_TO_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
  // BYTE*pDstYUV;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   
   //-- 3840x2160 0->1920X1080
   for(y=0;y<out_h;y++)
   {
		for(x =0;x<out_w;)
		{
			pDestBuf[0] = pSrcBuf[0];//y
			pDestBuf[1] = pSrcBuf[1];//cb

			pDestBuf[2] = pSrcBuf[4];//y
			pDestBuf[3] = pSrcBuf[3];//cr

			x = x+ 2;
			pSrcBuf +=8;
			pDestBuf +=4;
		}
		pSrcBuf += in_w*2;
   }
}
static void UHD_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   DWORD *pSrcData;
   DWORD *pDestData;
   BYTE*pSrcTmp;
   BYTE*pDestTmp;
   
   pSrcBuf = pSrc;
   pDestData = (DWORD *) pOut;
   //----------3840x2160->1280x720
   for(y=0;y<in_h;y++)
   {
	  if(y%3 == 0)
	  {
		  for(x=0;x<in_w;)
		  {
			 pSrcData =(DWORD *)pSrcBuf;
			*pDestData = *pSrcData;
			 pDestTmp =(BYTE *)pDestData;
			 pSrcData++;
			 pSrcTmp =(BYTE *)pSrcData;
			 pDestTmp[2] =  pSrcTmp[2];
			 pDestTmp[3] =  pSrcTmp[3];
			 
			pDestData +=1;
			pSrcBuf +=12;
			x= x+6;
		  }
	  }
	  else
	  {
		pSrcBuf += in_w*2;	
	  }

   }

}

static  void UHD_To_2560x1440_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   DWORD *pSrcData;
   DWORD *pDestData;
   BYTE*pSrcTmp;
   BYTE*pDestTmp;
   
   pSrcBuf = pSrc;
   pDestData = (DWORD *) pOut;
   //----------3840x2160->2560X1440
   for(y=0;y<in_h;y++)
   {
	  switch(y%6)
	  {
		case 0:
		case 2:
		case 4:
		case 5:
		{
		  for(x=0;x<in_w;)
		  {
			 pSrcData =(DWORD *)pSrcBuf;
			*pDestData = *pSrcData;
			 pDestTmp =(BYTE *)pDestData;
			 pSrcData++;
			 pSrcTmp =(BYTE *)pSrcData;
			 pDestTmp[2] =  pSrcTmp[0];
			 pDestTmp[3] =  pSrcTmp[3];
			 pDestData +=1;
			 
			 pSrcData++;
			 *pDestData = *pSrcData;
			 pDestData +=1;

			 pSrcBuf +=12;
			 x= x+6;
		  }
		  break;
	  	}
	  	case 1:
		case 3:
	  	{
			pSrcBuf += in_w*2;	
			break;
	  	}
   	}

   }

}
static void UHDW_To_2560X1440_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   DWORD *pSrcData;
   DWORD *pDestData;
   BYTE*pSrcTmp;
   BYTE*pDestTmp;

   pSrcBuf = pSrc;
   pDestData = (DWORD *) pOut;
   //----------4096x2160->2560x1440
   for(y=0;y<in_h;y++)
   {
	   switch(y%6)
	  {
		case 0:
		case 2:
		case 4:
		case 5:
	  	{
		  pSrcData = (DWORD *)(pSrcBuf + 128*2);
		  for(x=0;x<(in_w-256);)
		  {
			*pDestData = *pSrcData;
			 pDestTmp =(BYTE *)pDestData;
			 pSrcData++;
			 pSrcTmp =(BYTE *)pSrcData;
			 pDestTmp[2] =  pSrcTmp[0];
			 pDestTmp[3] =  pSrcTmp[3];
			 pDestData +=1;
			 pSrcData++;
			 *pDestData = *pSrcData;

			pDestData +=1;
			pSrcData +=2;
			x= x+6;
		  }
		  
		  pSrcBuf += in_w*2;	
		  break;
		}
	   case 1:
	   case 3:
	  	{
		 	pSrcBuf += in_w*2;	
			break;
	 	 }
	   }

   }

}
static  void UHDW_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   DWORD *pSrcData;
   DWORD *pDestData;
   BYTE*pSrcTmp;
   BYTE*pDestTmp;

   pSrcBuf = pSrc;
   pDestData = (DWORD *) pOut;
   //----------4096x2160->1280x720
   for(y=0;y<in_h;y++)
   {
	  if(y%3 == 0)
	  {
		  pSrcData = (DWORD *)(pSrcBuf + 128*2);
		  for(x=0;x<(in_w-256);)
		  {
			*pDestData = *pSrcData;
			 pDestTmp =(BYTE *)pDestData;
			 pSrcData++;
			 pSrcTmp =(BYTE *)pSrcData;
			 pDestTmp[2] =  pSrcTmp[0];
			 pDestTmp[3] =  pSrcTmp[3];
			 
			pDestData +=1;
			pSrcData +=2;
			x= x+6;
		  }
		  pSrcBuf += in_w*2;	
	  }
	  else
	  {
		 pSrcBuf += in_w*2;	
	  }

   }

}

static void UHDW_TO_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   BYTE*pSrcData;
   //BYTE*pDstYUV;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
  
   //-- 4096x2160 0->1920X1080
   for(y=0;y<out_h;y++)
   {
		pSrcData = pSrcBuf + 128*2;
		for(x =0;x<out_w;)
		{
			pDestBuf[0] = pSrcData[0];//y
			pDestBuf[1] = pSrcData[1];//cb

			pDestBuf[2] = pSrcData[4];//y
			pDestBuf[3] = pSrcData[3];//cr

			x = x+ 2;
			pSrcData +=8;
			pDestBuf +=4;
		}
		pSrcBuf += in_w*4;
   }
}

static  void V2560X1440_To_FHD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)

{
	   
	   	 int x,y;
		 BYTE*pSrcBuf;
		 BYTE*pDestBuf;
		 DWORD *pSrcData;
		 DWORD *pDestData;
		 BYTE*pSrcTmp;
		 BYTE*pDestTmp;
		 pDestData = (DWORD *) pOut;
		 pSrcData = (DWORD *) pSrc;
		 pSrcBuf = pSrc;
		 pDestBuf = pOut;
		 //2560X1440 -> 1920X1080
		 for(y=0;y<in_h;y++)
		 {
			
				if((y%4)==3)
				{
					pSrcBuf += in_w*2;
				}
				else
				{
					 pSrcData =  (DWORD *) pSrcBuf;
					 pDestData = (DWORD *) pDestBuf;
					for(x=0;x<in_w;)
					{
						*pDestData = *pSrcData;
						 pDestData ++;
						 pSrcData++;
						 
						*pDestData = *pSrcData;
						 pDestTmp =(BYTE*)pDestData;
						 pSrcData++;
						 pSrcTmp =(BYTE*)pSrcData;
						 pDestTmp[2] = pSrcTmp[0];
						 
						 pDestData ++;
						 pDestTmp =(BYTE*)pDestData;
						 pDestTmp[0] = pSrcTmp[2];
						 pDestTmp[1] = pSrcTmp[1];
						 pSrcData++;
						 pSrcTmp =(BYTE*)pSrcData;
						 pDestTmp[2] = pSrcTmp[2];
						 pDestTmp[3] = pSrcTmp[3];

						 pSrcData++;
						 pDestData++;
						 
						 x= x+8;
					}
					pDestBuf += out_w*2;
					pSrcBuf += in_w*2; 
				}
				
	   
		 }
	   

}

static  void V2560X1440_To_HD_Scaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
   int x,y;
   BYTE*pSrcBuf;
   BYTE*pDestBuf;
   //BYTE*pDstYUV;
   pSrcBuf = pSrc;
   pDestBuf = pOut;
   
   //-- V2560X1440->1280X720
   for(y=0;y<out_h;y++)
   {
		for(x =0;x<out_w;)
		{
			pDestBuf[0] = pSrcBuf[0];//y
			pDestBuf[1] = pSrcBuf[1];//cb

			pDestBuf[2] = pSrcBuf[4];//y
			pDestBuf[3] = pSrcBuf[3];//cr

			x = x+ 2;
			pSrcBuf +=8;
			pDestBuf +=4;
		}
		pSrcBuf += in_w*2;
   }
}

//---------------------------------
static void VideoScaler(BYTE *pSrc,BYTE *pOut,int in_w,int in_h,int out_w,int out_h)
{
	if((in_w == 1920)&&(in_h==1080)&&(out_w==1280)&&(out_h==720))
	{
		FHD_To_HD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1920)&&(in_h==1080)&&(out_w==800)&&(out_h==600))
	{
		FHD_To_800X600_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else  if((in_w == 3840)&&(in_h==2160)&&(out_w==800)&&(out_h==600))
	{
		UHD_To_800X600_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else  if((in_w == 4096)&&(in_h==2160)&&(out_w==800)&&(out_h==600))
	{
		UHDW_To_800X600_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1280)&&(in_h==720)&&(out_w==1920)&&(out_h==1080))
	{
		HD_To_FHD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1280)&&(in_h==720)&&(out_w==800)&&(out_h==600))
	{
		HD_To_800X600_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 720)&&(in_h==576)&&(out_w==720)&&(out_h==480))
	{
		SD_PAL_To_SD_NTSC_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 720)&&(in_h==480)&&(out_w==720)&&(out_h==576))
	{
		SD_NTSC_To_SD_PAL_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 720)&&(in_h==576)&&(out_w==1920)&&(out_h==1080))
	{
		SD_PAL_To_FHD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 720)&&(in_h==480)&&(out_w==1920)&&(out_h==1080))
	{
		SD_NTSC_To_FHD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 720)&&(in_h==576)&&(out_w==1280)&&(out_h==720))
	{
		SD_PAL_To_HD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 720)&&(in_h==480)&&(out_w==1280)&&(out_h==720))
	{
		SD_NTSC_To_HD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1280)&&(in_h==1024)&&(out_w==1920)&&(out_h==1080))
	{
		V1280X1024_To_FHD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1280)&&(in_h==1024)&&(out_w==1280)&&(out_h==720))
	{
		V1280X1024_To_HD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1280)&&(in_h==1024)&&(out_w==800)&&(out_h==600))
	{
		V1280X1024_To_800X600_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1920)&&(in_h==1080)&&(out_w==720)&&(out_h==480))
	{
		FHD_To_SD_NTSC_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1920)&&(in_h==1080)&&(out_w==720)&&(out_h==576))
	{
		FHD_To_SD_PAL_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1280)&&(in_h==720)&&(out_w==720)&&(out_h==480))
	{
		HD_To_SD_NTSC_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1280)&&(in_h==720)&&(out_w==720)&&(out_h==576))
	{
		HD_To_SD_PAL_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1280)&&(in_h==1024)&&(out_w==720)&&(out_h==480))
	{
		V1280X1024_NTSC_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 1280)&&(in_h==1024)&&(out_w==720)&&(out_h==576))
	{
		V1280X1024_PAL_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 3840)&&(in_h==2160)&&(out_w==1920)&&(out_h==1080))
	{
		UHD_TO_FHD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 3840)&&(in_h==2160)&&(out_w==2560)&&(out_h==1440))
	{
		UHD_To_2560x1440_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 3840)&&(in_h==2160)&&(out_w==1280)&&(out_h==720))
	{
		UHD_To_HD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 4096)&&(in_h==2160)&&(out_w==1920)&&(out_h==1080))
	{
		UHDW_TO_FHD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 4096)&&(in_h==2160)&&(out_w==1280)&&(out_h==720))
	{
		UHDW_To_HD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 4096)&&(in_h==2160)&&(out_w==2560)&&(out_h==1440))
	{
		UHDW_To_2560X1440_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 2560)&&(in_h==1440)&&(out_w==1920)&&(out_h==1080))
	{
		V2560X1440_To_FHD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 2560)&&(in_h==1440)&&(out_w==1280)&&(out_h==720))
	{
		V2560X1440_To_HD_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else if((in_w == 2560)&&(in_h==1440)&&(out_w==800)&&(out_h==600))
	{
		V2560X1440_To_800X600_Scaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
	else
	{
		All_VideoScaler(pSrc,pOut,in_w,in_h,out_w,out_h);
	}
}
static int  MemCopyFrame(int nDecoder,BYTE * dest,int nWidth,int nHeight,int interlace,BYTE *src[4],int len[4])
{
	int nCopySize[4];
	int h;
	int line_cnt;
	int res_size;
	BYTE *pSrcBuf;
	int hf_size;
	
	nCopySize[0] = len[0];
	nCopySize[1] = len[1];
	nCopySize[2] = len[2];
	nCopySize[3] = len[3];
	if(interlace ==0)
	{
		nCopySize[3] = nWidth*2*nHeight - nCopySize[0]-nCopySize[1]-nCopySize[2];
		if(nCopySize[3]<0)
		{
			return -1;
		}
		hf_size = (nWidth*nHeight*2)/(4*16*128);
		hf_size = hf_size*16*128;
		if(hf_size != len[0])
		{
			//DbgPrint("[LT] nCopySize[0] = %d ?= %d W=%d H=%d \n",nCopySize[0],hf_size,nWidth,nHeight);
			return -1;
		}
		memcpy(dest,src[0],nCopySize[0]); //
		dest +=nCopySize[0]; 
		memcpy(dest,src[1],nCopySize[1]); //
		dest +=nCopySize[1]; 
		memcpy(dest,src[2],nCopySize[2]); //
		dest +=nCopySize[2]; 	
		memcpy(dest,src[3],nCopySize[3]); //	
		//---------------------------

	}
	else
	{
		//-----copy buf 0---
		nCopySize[3] = nWidth*nHeight - nCopySize[0]-nCopySize[1]-nCopySize[2];
		if(nCopySize[3]<0)
		{
			return -1;
		}
		hf_size = (nWidth*nHeight)/(4*16*128);
		hf_size = hf_size*16*128;
		if(hf_size != len[0])
		{
			//DbgPrint("[LT] nCopySize[0] = %d ?= %d W=%d H=%d \n",nCopySize[0],hf_size,nWidth,nHeight);
			return -1;
		}
		
		pSrcBuf = src[0];
		line_cnt = nCopySize[0]/(nWidth*2);
		res_size = nCopySize[0]- (line_cnt*nWidth*2);
		if(res_size <0)
		{
			return -1;
		}
		//DbgPrint("MemCopyFrame bufer0 nCopySize[0]= %d  res_size=%d line_cnt =%d \n",nCopySize[0],res_size,line_cnt);
		for(h=0; h <line_cnt; h++)
		{
				if ((h & 0x3f) == 0)
					cond_resched();
			memcpy(dest,pSrcBuf,nWidth*2);
			dest += nWidth*2;
			memcpy(dest,pSrcBuf,nWidth*2);
			pSrcBuf += nWidth*2;
			dest += nWidth*2;
		}
		if(res_size >0)
		{
			memcpy(dest,pSrcBuf,res_size);
			pSrcBuf = src[1];
			dest +=res_size;
			memcpy(dest,pSrcBuf,(nWidth*2-res_size));
			dest += nWidth*2-res_size;
			memcpy(dest,dest-nWidth*2,nWidth*2);
			dest += nWidth*2;
			pSrcBuf += nWidth*2-res_size;
			nCopySize[1] -= nWidth*2-res_size;
		}
		else
		{
			pSrcBuf = src[1];
		}
		//-----copy buf 1---
		line_cnt = nCopySize[1]/(nWidth*2);
		res_size = nCopySize[1]- (line_cnt*nWidth*2);
		if(res_size <0)
		{
			
			return -1;
		}
		//DbgPrint("MemCopyFrame bufer1 nCopySize[1]= %d  res_size=%d line_cnt =%d \n",nCopySize[1],res_size,line_cnt);
		for(h=0; h <line_cnt; h++)
		{
				if ((h & 0x3f) == 0)
					cond_resched();
			memcpy(dest,pSrcBuf,nWidth*2);
			dest += nWidth*2;
			memcpy(dest,pSrcBuf,nWidth*2);
			pSrcBuf += nWidth*2;
			dest += nWidth*2;
		}
		if(res_size >0)
		{
			memcpy(dest,pSrcBuf,res_size);
			pSrcBuf = src[2];
			dest +=res_size;
			memcpy(dest,pSrcBuf,(nWidth*2-res_size));
			dest += nWidth*2-res_size;
			memcpy(dest,dest-nWidth*2,nWidth*2);
			dest += nWidth*2;
			pSrcBuf += nWidth*2-res_size;
			nCopySize[2] -= nWidth*2-res_size;
		}
		else
		{
			pSrcBuf = src[2];
		}

		//-----copy buf 2---
		line_cnt = nCopySize[2]/(nWidth*2);
		res_size = nCopySize[2]- line_cnt*nWidth*2;
		if(res_size <0)
		{
			
			return -1;
		}
		//DbgPrint("MemCopyFrame bufer2 nCopySize[2]= %d  res_size=%d line_cnt =%d \n",nCopySize[2],res_size,line_cnt);
		for(h=0; h <line_cnt; h++)
		{
				if ((h & 0x3f) == 0)
					cond_resched();
			memcpy(dest,pSrcBuf,nWidth*2);
			dest += nWidth*2;
			memcpy(dest,pSrcBuf,nWidth*2);
			pSrcBuf += nWidth*2;
			dest += nWidth*2;
		}
		if(res_size >0)
		{
			memcpy(dest,pSrcBuf,res_size);
			pSrcBuf = src[3];
			dest +=res_size;
			memcpy(dest,pSrcBuf,(nWidth*2-res_size));
			dest += nWidth*2-res_size;
			memcpy(dest,dest-nWidth*2,nWidth*2);
			dest += nWidth*2;
			pSrcBuf += nWidth*2-res_size;
			nCopySize[3] -= nWidth*2-res_size;
		}
		else
		{
			pSrcBuf = src[3];
		}
		//-----copy buf 3---
		line_cnt = nCopySize[3]/(nWidth*2);
		res_size = nCopySize[3]- line_cnt*nWidth*2;

		//DbgPrint("MemCopyFrame bufer3 nCopySize[3]= %d  res_size=%d line_cnt =%d \n",nCopySize[3],res_size,line_cnt);
		for(h=0; h <line_cnt; h++)
		{
				if ((h & 0x3f) == 0)
					cond_resched();
			memcpy(dest,pSrcBuf,nWidth*2);
			dest += nWidth*2;
			memcpy(dest,(dest-nWidth*2),nWidth*2);
			pSrcBuf += nWidth*2;
			dest += nWidth*2;
		}
		
		//-------------------
		
	}
	return 0;
}

//--------------------------------
static struct hwsvideo_buffer *hws_pop_any_buffer(struct hws_video *videodev)
{
    struct hws_vfh_ctx *ctx;
    struct hwsvideo_buffer *buf = NULL;
    unsigned long flags;

    spin_lock_irqsave(&videodev->consumers_lock, flags);
    list_for_each_entry(ctx, &videodev->consumers, node) {
        unsigned long qflags;

        if (!ctx->streaming)
            continue;

        spin_lock_irqsave(&ctx->qlock, qflags);
        if (!list_empty(&ctx->buf_queue)) {
            buf = list_first_entry(&ctx->buf_queue, struct hwsvideo_buffer, queue);
            list_del(&buf->queue);
            /* Round-robin fairness across active consumers. */
            list_move_tail(&ctx->node, &videodev->consumers);
            spin_unlock_irqrestore(&ctx->qlock, qflags);
            break;
        }
        spin_unlock_irqrestore(&ctx->qlock, qflags);
    }
    spin_unlock_irqrestore(&videodev->consumers_lock, flags);

    return buf;
}

static bool __maybe_unused hws_has_pending_buffers(struct hws_video *videodev)
{
    struct hws_vfh_ctx *ctx;
    unsigned long flags;
    bool pending = false;

    spin_lock_irqsave(&videodev->consumers_lock, flags);
    list_for_each_entry(ctx, &videodev->consumers, node) {
        unsigned long qflags;

        if (!ctx->streaming)
            continue;

        spin_lock_irqsave(&ctx->qlock, qflags);
        if (!list_empty(&ctx->buf_queue)) {
            pending = true;
            spin_unlock_irqrestore(&ctx->qlock, qflags);
            break;
        }
        spin_unlock_irqrestore(&ctx->qlock, qflags);
    }
    spin_unlock_irqrestore(&videodev->consumers_lock, flags);
    return pending;
}

static unsigned int hws_pending_consumer_count(struct hws_video *videodev)
{
    struct hws_vfh_ctx *ctx;
    unsigned long flags;
    unsigned int count = 0;

    spin_lock_irqsave(&videodev->consumers_lock, flags);
    list_for_each_entry(ctx, &videodev->consumers, node) {
        unsigned long qflags;

        if (!ctx->streaming)
            continue;

        spin_lock_irqsave(&ctx->qlock, qflags);
        if (!list_empty(&ctx->buf_queue))
            count++;
        spin_unlock_irqrestore(&ctx->qlock, qflags);
    }
    spin_unlock_irqrestore(&videodev->consumers_lock, flags);

    return count;
}

static u64 hws_next_frame_ts_ns(struct hws_video *videodev, u64 now_ns)
{
	u64 prev = READ_ONCE(videodev->next_frame_ts_ns);
	u64 interval_ns;
	int fps = hws_policy_select_fps(videodev, 60);

	if (fps <= 0)
		fps = 60;
	interval_ns = DIV_ROUND_CLOSEST_ULL(HWS_NS_PER_SEC, (u64)fps);
	if (interval_ns == 0)
		interval_ns = 16666667ULL;

	if (!prev || now_ns > prev + interval_ns * 4)
		prev = now_ns;
	else
		prev += interval_ns;

	WRITE_ONCE(videodev->next_frame_ts_ns, prev);
	return prev;
}

static void video_data_process(struct work_struct *p_work)
{
	struct hws_video *videodev = container_of(p_work, struct hws_video, videowork);
	unsigned long devflags;
	int nVindex = -1;
	int i;
	int in_width;
	int in_height;
	int in_vsize;
	int out_width;
	int out_height;
	int out_size = 0;
	bool needs_scaler;
	BYTE *bBuf[4];
	int nCopySize[4];
	int interlace = 0;
	int miss_freme = 0;
	int curr_no_video;
	struct hws_pcie_dev *pdx = videodev->dev;
	int nCh;
	bool diag_on = !!diag_enable;
	u64 work_start_ns = 0;
	u64 work_elapsed_ns = 0;
	u64 memcopy_ns = 0;
	u64 scaler_ns = 0;
	u64 buf_processed = 0;
	u64 buf_done = 0;
	u64 buf_error = 0;
	u64 copy_frames = 0;
	u64 scaler_frames = 0;
	u64 novideo_frames = 0;
	unsigned int budget;
	unsigned int processed_in_run = 0;
	bool budget_exhausted = false;
	unsigned int per_frame_budget = 1;
	u64 frame_ts_ns = 0;
	bool had_fresh_frame = false;
	bool no_fresh_frame = false;
	u64 source_interval_ns = 0;
	unsigned int output_frames_target = 1;
	int source_fps = 0;
	int target_fps = 0;
	int policy_mode = HWS_FPS_POLICY_AUTO_PROFILE;

	bBuf[0] = NULL;
	bBuf[1] = NULL;
	bBuf[2] = NULL;
	bBuf[3] = NULL;
	nCopySize[0] = 0;
	nCopySize[1] = 0;
	nCopySize[2] = 0;
	nCopySize[3] = 0;
	nCh = videodev->index;
	budget = (video_work_budget > 0) ? (unsigned int)video_work_budget : 1U;

	if (diag_on)
		work_start_ns = ktime_get_ns();

	/* Hold the device lock only while selecting/snapshotting source buffers. */
	spin_lock_irqsave(&pdx->videoslock[nCh], devflags);
	in_width = pdx->m_pVCAPStatus[nCh][0].dwWidth;
	in_height = pdx->m_pVCAPStatus[nCh][0].dwHeight;
	if (pdx->m_pVCAPStatus[nCh][0].dwinterlace == 1)
		in_height = in_height * 2;
	in_vsize = in_width * in_height * 2;
	curr_no_video = pdx->m_curr_No_Video[nCh];

	if (curr_no_video == 0) {
		nVindex = -1;
		if (pdx->m_VideoInfo[nCh].pStatusInfo[pdx->m_nRDVideoIndex[nCh]].byLock == MEM_LOCK) {
			nVindex = pdx->m_nRDVideoIndex[nCh];
			bBuf[0] = pdx->m_VideoInfo[nCh].m_pVideoBufData[nVindex];
			bBuf[1] = pdx->m_VideoInfo[nCh].m_pVideoBufData1[nVindex];
			bBuf[2] = pdx->m_VideoInfo[nCh].m_pVideoBufData2[nVindex];
			bBuf[3] = pdx->m_VideoInfo[nCh].m_pVideoBufData3[nVindex];
			nCopySize[0] = pdx->m_VideoInfo[nCh].m_VideoBufferSize[0];
			nCopySize[1] = pdx->m_VideoInfo[nCh].m_VideoBufferSize[1];
			nCopySize[2] = pdx->m_VideoInfo[nCh].m_VideoBufferSize[2];
			nCopySize[3] = pdx->m_VideoInfo[nCh].m_VideoBufferSize[3];
			interlace = pdx->m_VideoInfo[nCh].pStatusInfo[nVindex].dwinterlace;
		}
		if (nVindex == -1)
			miss_freme = 1;
	}
	if (curr_no_video == 0) {
		if (nVindex >= 0) {
			u64 now_ns = ktime_get_ns();
			had_fresh_frame = true;
			if (hws_diag_last_fresh_ns[nCh] != 0 && now_ns > hws_diag_last_fresh_ns[nCh])
				source_interval_ns = now_ns - hws_diag_last_fresh_ns[nCh];
			hws_diag_last_fresh_ns[nCh] = now_ns;
			if (source_interval_ns > 0) {
				u64 prev_avg = READ_ONCE(hws_source_interval_ns_avg[nCh]);
				u64 new_avg = prev_avg ? ((prev_avg * 7) + source_interval_ns) / 8 : source_interval_ns;
				WRITE_ONCE(hws_source_interval_ns_avg[nCh], new_avg);
			}
		} else {
			no_fresh_frame = true;
		}
	} else {
		for (i = 0; i < MAX_VIDEO_QUEUE; i++) {
			if (pdx->m_VideoInfo[nCh].pStatusInfo[i].byLock == MEM_LOCK)
				pdx->m_VideoInfo[nCh].pStatusInfo[i].byLock = MEM_UNLOCK;
		}
	}
	spin_unlock_irqrestore(&pdx->videoslock[nCh], devflags);

	out_width = videodev->current_out_width;
	out_height = videodev->curren_out_height;
	out_size = out_width * out_height * 2;
	needs_scaler = (in_vsize != out_size);
	frame_ts_ns = 0;
	per_frame_budget = hws_pending_consumer_count(videodev);
	if (per_frame_budget == 0)
		per_frame_budget = 1;
	if (per_frame_budget > budget)
		per_frame_budget = budget;

	source_fps = hws_effective_source_fps(videodev);
	target_fps = videodev->current_out_framerate;
	if (target_fps <= 0)
		target_fps = 60;
	policy_mode = hws_effective_policy_mode();
	if (policy_mode == HWS_FPS_POLICY_DRIVER_CONVERT_EXPERIMENTAL && had_fresh_frame && source_fps > 0) {
		videodev->output_rate_accum += target_fps;
		output_frames_target = videodev->output_rate_accum / source_fps;
		videodev->output_rate_accum %= source_fps;
		if (target_fps >= source_fps) {
			if (output_frames_target == 0)
				output_frames_target = 1;
			if (output_frames_target > 4)
				output_frames_target = 4;
		} else {
			if (output_frames_target > 1)
				output_frames_target = 1;
		}
	} else {
		output_frames_target = 1;
		if (policy_mode != HWS_FPS_POLICY_DRIVER_CONVERT_EXPERIMENTAL)
			videodev->output_rate_accum = 0;
	}

	if (per_frame_budget * output_frames_target > budget)
		per_frame_budget = budget;
	else
		per_frame_budget *= output_frames_target;

	if (curr_no_video == 0 && nVindex < 0)
		goto out_finish;

	for (;;) {
		struct hwsvideo_buffer *buf;
		int copy_ret = 0;
		u64 copy_start;
		unsigned int dst_capacity;
		unsigned int required_size;

		if (processed_in_run >= per_frame_budget) {
			budget_exhausted = true;
			break;
		}

		buf = hws_pop_any_buffer(videodev);
		if (!buf)
			break;

		processed_in_run++;
		buf_processed++;

		frame_ts_ns = hws_next_frame_ts_ns(videodev, ktime_get_ns());
		buf->vb.vb2_buf.timestamp = frame_ts_ns;
		buf->vb.field = V4L2_FIELD_NONE;

		if (!buf->mem) {
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
			buf_error++;
			continue;
		}

		dst_capacity = vb2_plane_size(&buf->vb.vb2_buf, 0);
		required_size = (curr_no_video == 0 && !needs_scaler) ?
			(unsigned int)in_vsize : (unsigned int)out_size;
		if (dst_capacity < required_size) {
			if (diag_on) {
				pr_warn_ratelimited(
					"hws: vb2 dst too small ch=%d cap=%u need=%u no_video=%d scaler=%d\n",
					nCh, dst_capacity, required_size, curr_no_video, needs_scaler);
			}
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
			buf_error++;
			continue;
		}

		if (curr_no_video == 0) {
			if (needs_scaler) {
				if (!pdx->m_VideoInfo[nCh].m_pVideoScalerBuf) {
					copy_ret = -ENOMEM;
				} else {
					copy_start = diag_on ? ktime_get_ns() : 0;
					copy_ret = MemCopyFrame(nCh, pdx->m_VideoInfo[nCh].m_pVideoScalerBuf,
						in_width, in_height, interlace, bBuf, nCopySize);
					if (diag_on)
						memcopy_ns += ktime_get_ns() - copy_start;
					if (!copy_ret) {
						copy_start = diag_on ? ktime_get_ns() : 0;
						VideoScaler(pdx->m_VideoInfo[nCh].m_pVideoScalerBuf, buf->mem,
							in_width, in_height, out_width, out_height);
						if (diag_on)
							scaler_ns += ktime_get_ns() - copy_start;
						scaler_frames++;
					}
				}
			} else {
				copy_start = diag_on ? ktime_get_ns() : 0;
				copy_ret = MemCopyFrame(nCh, buf->mem, in_width, in_height,
					interlace, bBuf, nCopySize);
				if (diag_on)
					memcopy_ns += ktime_get_ns() - copy_start;
				if (!copy_ret)
					copy_frames++;
			}
			if (copy_ret) {
				vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
				buf_error++;
				continue;
			}
		} else {
			SetNoVideoMem(buf->mem, out_width, out_height);
			novideo_frames++;
		}

		buf->vb.sequence = videodev->seqnr++;
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		buf_done++;

		if ((processed_in_run & 0x3U) == 0)
			cond_resched();
	}

	spin_lock_irqsave(&pdx->videoslock[nCh], devflags);
	if (curr_no_video == 0 && nVindex >= 0) {
		pdx->m_VideoInfo[nCh].pStatusInfo[nVindex].byLock = MEM_UNLOCK;
		pdx->m_nRDVideoIndex[nCh] = nVindex + 1;
		if (pdx->m_nRDVideoIndex[nCh] >= MAX_VIDEO_QUEUE)
			pdx->m_nRDVideoIndex[nCh] = 0;
	}
	spin_unlock_irqrestore(&pdx->videoslock[nCh], devflags);

out_finish:
	if (diag_on) {
		work_elapsed_ns = ktime_get_ns() - work_start_ns;
		atomic64_inc(&hws_diag[nCh].work_runs);
		atomic64_add(work_elapsed_ns, &hws_diag[nCh].work_ns_total);
		hws_diag_update_max(&hws_diag[nCh].work_ns_max, work_elapsed_ns);
		atomic64_add(buf_processed, &hws_diag[nCh].buf_processed);
		atomic64_add(buf_done, &hws_diag[nCh].buf_done);
		atomic64_add(buf_error, &hws_diag[nCh].buf_error);
		atomic64_add(copy_frames, &hws_diag[nCh].copy_path_frames);
		atomic64_add(scaler_frames, &hws_diag[nCh].scaler_path_frames);
		atomic64_add(novideo_frames, &hws_diag[nCh].novideo_frames);
		if (miss_freme)
			atomic64_inc(&hws_diag[nCh].miss_frame_fallbacks);
		atomic64_add(memcopy_ns, &hws_diag[nCh].memcopy_ns_total);
		atomic64_add(scaler_ns, &hws_diag[nCh].scaler_ns_total);
		if (had_fresh_frame)
			atomic64_inc(&hws_diag[nCh].fresh_frame_runs);
		if (no_fresh_frame)
			atomic64_inc(&hws_diag[nCh].nofresh_frame_runs);
		if (source_interval_ns > 0) {
			atomic64_add(source_interval_ns, &hws_diag[nCh].source_interval_ns_total);
			hws_diag_update_max(&hws_diag[nCh].source_interval_ns_max, source_interval_ns);
			atomic64_inc(&hws_diag[nCh].source_interval_samples);
		}
	}

	(void)budget_exhausted;
	(void)miss_freme;
}

static void hws_get_video_param(struct hws_pcie_dev *dev,int index)
{
	
	//printk( "%s(): %x \n", __func__, index);
	int width,height;
	width= dev->m_pVCAPStatus[index][0].dwWidth;
	height=dev->m_pVCAPStatus[index][0].dwHeight;
	dev->video[index].current_out_pixfmt =0;
	dev->video[index].current_out_size_index = 0;
	dev->video[index].current_out_width = width;
	dev->video[index].curren_out_height = height;
	dev->video[index].current_out_framerate = 60;
	dev->video[index].Interlaced = 0;
	//printk( "%s(%dx%d):  \n", __func__, width,height);

}

static void hws_adapters_init(struct hws_pcie_dev *dev)
{
  int i;
  for (i = 0; i <MAX_VID_CHANNELS; i++) {
		hws_get_video_param(dev,i);
	}
}
static void hws_remove_deviceregister(struct hws_pcie_dev *dev)
{
	int i;
	struct video_device *vdev ;
	for(i=0;i<dev->m_nCurreMaxVideoChl;i++)
	{
		vdev = &(dev->video[i].vdev);
		if(vdev)
		{
			v4l2_device_unregister(&dev->video[i].v4l2_dev);
			vdev = NULL;
		}
	}
}
static int hws_video_register(struct hws_pcie_dev *dev)
{
	struct video_device *vdev ;
	struct vb2_queue *q ;
	int i;
	int err=-1;
	//printk("hws_video_register Start\n");
	for(i=0;i<dev->m_nCurreMaxVideoChl;i++)
	{
			//printk("v4l2_device_register[%d]\n",i);
			err = v4l2_device_register(&dev->pdev->dev, &dev->video[i].v4l2_dev);
			if(err<0){
				printk(KERN_ERR " v4l2_device_register 0 error! \n");
				hws_remove_deviceregister(dev);
				return -1;
			}
	}
	//printk("v4l2_device_register end\n");
	//----------------------------------------------------
	for(i=0;i<dev->m_nCurreMaxVideoChl;i++){
		//printk("v4l2_device_register INT[%d]\n",i);
		vdev = &(dev->video[i].vdev);
		q = &(dev->video[i].vq);
		if (NULL == vdev){
			printk(KERN_ERR " video_device_alloc failed !!!!! \n");
			goto fail;
		}
		dev->video[i].index = i;
		dev->video[i].dev = dev;
		dev->video[i].fileindex =0;
		dev->video[i].startstreamIndex=0;
		dev->video[i].std = V4L2_STD_NTSC_M;
		dev->video[i].pixfmt = V4L2_PIX_FMT_YUYV;
		//-------------------
		dev->video[i].m_Curr_Brightness = BrightnessDefault;
		dev->video[i].m_Curr_Contrast   = ContrastDefault;
		dev->video[i].m_Curr_Saturation = SaturationDefault;
		dev->video[i].m_Curr_Hue = HueDefault; 
		//-------------------
		vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
		vdev->v4l2_dev = &(dev->video[i].v4l2_dev);
		vdev->lock = &(dev->video[i].video_lock);
		vdev->fops = &hws_fops;
		strscpy(vdev->name, KBUILD_MODNAME, sizeof(vdev->name));
		vdev->release = video_device_release_empty;
		vdev->vfl_dir = VFL_DIR_RX;
		vdev->ioctl_ops = &hws_ioctl_fops;
		mutex_init(&(dev->video[i].video_lock));
		mutex_init(&(dev->video[i].queue_lock));
		spin_lock_init(&dev->video[i].consumers_lock);
		mutex_init(&dev->video[i].ioctl_lock);
		dev->video[i].ioctl_owner = NULL;
		INIT_LIST_HEAD(&dev->video[i].consumers);
		atomic_set(&dev->video[i].engine_users, 0);

		spin_lock_init(&dev->video[i].slock);
		//printk("v4l2_device_register INT3[%d]\n",i);
		INIT_LIST_HEAD(&dev->video[i].queue);
		//printk("v4l2_device_register INT2[%d]\n",i);
		video_set_drvdata(vdev, &(dev->video[i]));
		
		q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		q->io_modes = VB2_READ | VB2_MMAP | VB2_USERPTR;
		//q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
		q->gfp_flags = GFP_DMA32;
		//q->min_buffers_needed = 2;
		q->drv_priv = &(dev->video[i]);
		q->buf_struct_size = sizeof(struct hwsvideo_buffer);
		q->ops = &hwspcie_video_qops;
		
		//q->mem_ops = &vb2_dma_contig_memops;
		//q->mem_ops = &vb2_dma_sg_memops;
		q->mem_ops = &vb2_vmalloc_memops;
		
		q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		
		q->lock = &(dev->video[i].queue_lock);
		q->dev = &(dev->pdev->dev);
		vdev->queue = q;	
		err = vb2_queue_init(q);
		if(err != 0){
			printk(KERN_ERR " vb2_queue_init failed !!!!! \n");
			goto fail;	
		}
		
		INIT_WORK(&dev->video[i].videowork,video_data_process);
		#if (LINUX_VERSION_CODE < KERNEL_VERSION(5,7,0))
		err = video_register_device(vdev, VFL_TYPE_GRABBER,-1);
		#else
		err = video_register_device(vdev, VFL_TYPE_VIDEO,-1);
		#endif
		if(err!=0){
			printk(KERN_ERR " v4l2_device_register failed !!!!! \n");
			goto fail;
		}else{
			//printk(" video_register_device OK !!!!! \n");
		}
	}
	//printk("hws_video_register End\n");
	return 0;
fail:
	for(i=0;i<dev->m_nCurreMaxVideoChl;i++){
		vdev = &dev->video[i].vdev;
		video_unregister_device(vdev);
		v4l2_device_unregister(&dev->video[i].v4l2_dev);
	}
	return err;
}

/* HDMI 0x39[3:0] - CS_DATA[27:24] 0 for reserved values*/
static const int cs_data_fs[] __maybe_unused = {
	44100,
	0,
	48000,
	32000,
	0,
	0,
	0,
	0,
	88200,
	768000,
	96000,
	0,
	176000,
	0,
	192000,
	0,
};
#if 1
static struct snd_pcm_hardware audio_pcm_hardware = {
    .info 				=	(SNDRV_PCM_INFO_MMAP |
                             SNDRV_PCM_INFO_INTERLEAVED |
                             SNDRV_PCM_INFO_BLOCK_TRANSFER |
                             SNDRV_PCM_INFO_RESUME |
                             SNDRV_PCM_INFO_MMAP_VALID),
    .formats 			=	SNDRV_PCM_FMTBIT_S16_LE,
    .rates 				=   SNDRV_PCM_RATE_48000,
    .rate_min 			=	48000,
    .rate_max 			=	48000,
    .channels_min 		=	2,
    .channels_max 		=	2,
    .buffer_bytes_max 	=	512*1024,
    .period_bytes_min 	=	1024,
    .period_bytes_max 	=	64*1024,
    .periods_min 		=	2,
    .periods_max 		=	64,
};
#else
static struct snd_pcm_hardware audio_pcm_hardware ={
	.info =  (SNDRV_PCM_INFO_INTERLEAVED |SNDRV_PCM_INFO_BLOCK_TRANSFER ),
	.formats = (SNDRV_PCM_FMTBIT_S16_LE),
	.rates = SNDRV_PCM_RATE_KNOT | SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max =2,
	.period_bytes_min = HWS_AUDIO_CELL_SIZE,
	.period_bytes_max = HWS_AUDIO_CELL_SIZE,
	.periods_min      = 4,
	.periods_max      = 4,
	.buffer_bytes_max = HWS_AUDIO_CELL_SIZE*4,
};
#endif
static int hws_pcie_audio_open(struct snd_pcm_substream *substream)
{
	struct hws_audio *drv = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int req_period_bytes;
	unsigned int req_periods;
	unsigned int req_period_max;
	unsigned int req_buffer_max;
	unsigned int req_period_count;

	
    drv->sample_rate_out        = 48000;
    drv->channels               = 2;
	//printk(KERN_INFO "%s() index:%x\n",__func__,drv->index);
    runtime->hw = audio_pcm_hardware;
	req_period_bytes = (audio_period_bytes > 0) ? (unsigned int)audio_period_bytes : 1024U;
	if (req_period_bytes < 1024U)
		req_period_bytes = 1024U;
	req_period_bytes &= ~3U;
	if (req_period_bytes == 0)
		req_period_bytes = 1024U;
	req_periods = (audio_periods > 0) ? (unsigned int)audio_periods : 8U;
	req_period_count = hws_audio_init_period_constraints(drv, req_period_bytes);
	if (req_period_count > 0) {
		req_period_bytes = drv->period_bytes_choices[0];
		req_period_max = drv->period_bytes_choices[req_period_count - 1];
	} else {
		req_period_max = req_period_bytes;
	}
	if (req_period_count == 0)
		req_period_max = req_period_bytes * 4U;
	if (req_period_max < req_period_bytes)
		req_period_max = req_period_bytes;
	req_buffer_max = req_period_max * req_periods;
	if (req_buffer_max < req_period_max * 2U)
		req_buffer_max = req_period_max * 2U;
	if (req_buffer_max > 512U * 1024U)
		req_buffer_max = 512U * 1024U;
	if (req_period_max > req_buffer_max)
		req_period_max = req_buffer_max;
	if (req_periods > 64U)
		req_periods = 64U;
	if (req_periods < 2U)
		req_periods = 2U;

	runtime->hw.period_bytes_min = req_period_bytes;
	runtime->hw.period_bytes_max = req_period_max;
	runtime->hw.buffer_bytes_max = req_buffer_max;
	runtime->hw.periods_min = 2U;
	runtime->hw.periods_max = req_periods;
	if (req_period_count > 0)
		snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					 &drv->period_bytes_constraint);
	snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
    WRITE_ONCE(drv->substream, substream);
	WRITE_ONCE(drv->last_irq_ns, 0);
	WRITE_ONCE(drv->last_copy_ns, 0);
	WRITE_ONCE(drv->last_progress_ns, 0);
	//snd_pcm_hw_constraint_minmax(runtime,SNDRV_PCM_HW_PARAM_RATE,setrate,setrate);
	return 0;
}

static int hws_pcie_audio_close(struct snd_pcm_substream *substream)
{
	struct hws_audio *drv = snd_pcm_substream_chip(substream);
	unsigned long flags;

	spin_lock_irqsave(&drv->ring_lock, flags);
	drv->ring_wpos_byframes = 0;
	drv->period_used_byframes = 0;
	drv->ring_size_byframes = 0;
	drv->period_size_byframes = 0;
	spin_unlock_irqrestore(&drv->ring_lock, flags);
	cancel_delayed_work_sync(&drv->silence_work);
	cancel_work_sync(&drv->audiowork);
	WRITE_ONCE(drv->last_irq_ns, 0);
	WRITE_ONCE(drv->last_copy_ns, 0);
	WRITE_ONCE(drv->last_progress_ns, 0);
	WRITE_ONCE(drv->substream, NULL);
	return 0;
} 
static int hws_pcie_audio_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
	//printk(KERN_INFO "%s() \n",__func__);
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}  

static int hws_pcie_audio_hw_free(struct snd_pcm_substream *substream)
{
	//printk(KERN_INFO "%s() \n",__func__);
	return snd_pcm_lib_free_pages(substream);
} 

static int hws_pcie_audio_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct hws_audio *drv = snd_pcm_substream_chip(substream);
	//struct hws_pcie_dev *dev= drv->dev;
	//int i;
	unsigned long flags;
	//printk(KERN_INFO "%s() index:%x\n",__func__,drv->index);
	
	spin_lock_irqsave(&drv->ring_lock, flags);
    drv->ring_size_byframes = runtime->buffer_size;
    drv->ring_wpos_byframes = 0;
    drv->period_size_byframes = runtime->period_size;
    drv->period_used_byframes = 0;
	drv->ring_offsize =0;
	drv->ring_over_size =0;
	WRITE_ONCE(drv->last_irq_ns, 0);
	WRITE_ONCE(drv->last_copy_ns, 0);
	WRITE_ONCE(drv->last_progress_ns, 0);
    spin_unlock_irqrestore(&drv->ring_lock, flags);
	
	return 0;
}  
static int hws_pcie_audio_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct hws_audio *chip = snd_pcm_substream_chip(substream);
	struct hws_pcie_dev *dev= chip->dev;
	unsigned long flags;
	switch(cmd){
		case SNDRV_PCM_TRIGGER_START:
			//HWS_PCIE_READ(HWS_DMA_BASE(chip->index), HWS_DMA_STATUS);
			//start dma
			//HWS_PCIE_WRITE(HWS_INT_BASE, HWS_DMA_MASK(chip->index), 0x00000001); 
			//HWS_PCIE_WRITE(HWS_DMA_BASE(chip->index), HWS_DMA_START, 0x00000001);
			//printk(KERN_INFO "SNDRV_PCM_TRIGGER_START index:%x\n",chip->index);	
	 					spin_lock_irqsave(&chip->ring_lock, flags);
			chip->ring_wpos_byframes = 0;
			chip->period_used_byframes = 0;
			spin_unlock_irqrestore(&chip->ring_lock, flags);
			WRITE_ONCE(chip->last_irq_ns, 0);
			WRITE_ONCE(chip->last_copy_ns, 0);
			WRITE_ONCE(chip->last_progress_ns, ktime_get_ns());
			cancel_delayed_work(&chip->silence_work);
			StartAudioCapture(dev,chip->index);
			queue_delayed_work(dev->auwq, &chip->silence_work,
					   hws_audio_fallback_delay_jiffies(chip, 0));
			break;
		case SNDRV_PCM_TRIGGER_STOP:
			//stop dma
			//HWS_PCIE_WRITE(HWS_INT_BASE, HWS_DMA_MASK(chip->index), 0x000000000); 
			//HWS_PCIE_WRITE(HWS_DMA_BASE(chip->index), HWS_DMA_START, 0x00000000);
			//printk(KERN_INFO "SNDRV_PCM_TRIGGER_STOP index:%x\n",chip->index);
			StopAudioCapture(dev,chip->index);
			/* Do not sleep in trigger path: can run under atomic constraints. */
			cancel_delayed_work(&chip->silence_work);
			cancel_work(&chip->audiowork);
			spin_lock_irqsave(&chip->ring_lock, flags);
			chip->ring_wpos_byframes = 0;
			chip->period_used_byframes = 0;
			spin_unlock_irqrestore(&chip->ring_lock, flags);
			WRITE_ONCE(chip->last_irq_ns, 0);
			WRITE_ONCE(chip->last_copy_ns, 0);
			WRITE_ONCE(chip->last_progress_ns, 0);
			break;
		default:
			return -EINVAL;
			break;
	}
	return 0;
}  
//-------------------------------------------------


//-------------------------------------------------
static snd_pcm_uframes_t hws_pcie_audio_pointer(struct snd_pcm_substream *substream)
{
	struct hws_audio *drv = snd_pcm_substream_chip(substream);
	//struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t pos;
	unsigned long flags;
	//printk(KERN_INFO "%s() index:%x\n", __func__, drv->index);
	if (!READ_ONCE(drv->substream))
		return 0;
	spin_lock_irqsave(&drv->ring_lock,flags);  //spin_lock
    pos = drv->ring_wpos_byframes;
    spin_unlock_irqrestore(&drv->ring_lock,flags); //spin_unlock
	 return pos;
}

struct snd_pcm_ops hws_pcie_pcm_ops ={
	.open =			hws_pcie_audio_open,
	.close = 		hws_pcie_audio_close,
	.ioctl =		snd_pcm_lib_ioctl,
	.hw_params = 	hws_pcie_audio_hw_params,
	.hw_free =		hws_pcie_audio_hw_free,
	.prepare =		hws_pcie_audio_prepare,
	.trigger =		hws_pcie_audio_trigger,
	.pointer =		hws_pcie_audio_pointer
};

static int hws_audio_register(struct hws_pcie_dev *dev)
{
	struct snd_pcm		*pcm;
	struct snd_card 	*card;
	int ret;
	int i;
	int ai_index;
	char audioname[100];	  
	//printk("hws_audio_register Start\n");
	ai_index = dev->m_Device_PortID*dev->m_nCurreMaxVideoChl+1;
	for(i=0;i<dev->m_nCurreMaxVideoChl;i++){
		scnprintf(audioname, sizeof(audioname), "%s %d", HWS_AUDOI_NAME, i + ai_index);
		//printk("%s\n",audioname);
		ret = snd_card_new(&dev->pdev->dev, -1, audioname, THIS_MODULE,	sizeof(struct hws_audio), &card);
	   // ret = snd_card_new(&dev->pdev->dev, audio_index[i], audio_id[i], THIS_MODULE,	sizeof(struct hws_audio), &card);
		if (ret < 0){
			printk(KERN_ERR "%s() ERROR: snd_card_new failed <%d>\n",__func__, ret);
			goto fail0;
		}
		strscpy(card->driver, KBUILD_MODNAME, sizeof(card->driver));
		strscpy(card->shortname, audioname, sizeof(card->shortname));
		strscpy(card->longname, card->shortname, sizeof(card->longname));

		ret = snd_pcm_new(card,audioname,0,0,1,&pcm);
		if (ret < 0){
			printk(KERN_ERR "%s() ERROR: snd_pcm_new failed <%d>\n",__func__, ret);
			goto fail1;
		}
		dev->audio[i].index=i;
		dev->audio[i].dev=dev;
		pcm->private_data = &dev->audio[i];	
		strscpy(pcm->name, audioname, sizeof(pcm->name));
		snd_pcm_set_ops(pcm,SNDRV_PCM_STREAM_CAPTURE,&hws_pcie_pcm_ops);
		//snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_DEV,snd_dma_pci_data(dev->pdev), HWS_AUDIO_CELL_SIZE*4, HWS_AUDIO_CELL_SIZE*4);
		 snd_pcm_lib_preallocate_pages_for_all(
            pcm,
            SNDRV_DMA_TYPE_CONTINUOUS,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,19,0))
			card->dev,
#else
            snd_dma_continuous_data(GFP_KERNEL),
#endif
            audio_pcm_hardware.buffer_bytes_max,
            audio_pcm_hardware.buffer_bytes_max
            );
		//----------------------------
		  dev->audio[i].sample_rate_out        = 48000;
    	  dev->audio[i].channels               = 2;
		  dev->audio[i].resampled_buf_size = dev->audio[i].sample_rate_out * 2/* sample bytes */ * dev->audio[i].channels /* channels */;
		  //dev->audio[i].resampled_buf = vmalloc(dev->audio[i].resampled_buf_size);  
		//if(dev->audio[i].resampled_buf == NULL)
		//	goto fail1;
		//-----------------
		spin_lock_init(&dev->audio[i].ring_lock);
		INIT_WORK(&dev->audio[i].audiowork,audio_data_process);
		INIT_DELAYED_WORK(&dev->audio[i].silence_work, hws_audio_silence_fallback_work);
		ret = snd_card_register(card);
		if ( ret < 0) {
			printk(KERN_ERR "%s() ERROR: snd_card_register failed\n",__func__);
			goto fail1;
		}
		dev->audio[i].card =card;
	}
	//printk("hws_audio_register End\n");
	return 0;
fail1:
	for(i=0;i<dev->m_nCurreMaxVideoChl;i++){
		
		if(dev->audio[i].card)
		{
			snd_card_free(dev->audio[i].card);
			dev->audio[i].card=NULL;
		}
		if(dev->audio[i].resampled_buf)
		{
			vfree(dev->audio[i].resampled_buf);
			dev->audio[i].resampled_buf = NULL;
		}
	}
fail0:
	return -1;
}
//-------------------
//static unsigned long video_data[MAX_VID_CHANNELS];
//static struct tasklet_struct dpc_video_tasklet[MAX_VID_CHANNELS];
//static unsigned long audio_data[MAX_VID_CHANNELS];
//static struct tasklet_struct dpc_audio_tasklet[MAX_VID_CHANNELS];



static void  WRITE_REGISTER_ULONG (struct hws_pcie_dev *pdx,u32 RegisterOffset,u32 Value)
{
	//map_bar0_addr[RegisterOffset/4] = Value;
	char *bar0;
	bar0 = (char*)pdx->map_bar0_addr;
	iowrite32(Value,bar0+RegisterOffset);
	//map_bar0_addr[RegisterOffset/4] = Value;

}

static u32 READ_REGISTER_ULONG (struct hws_pcie_dev *pdx,u32 RegisterOffset)
{
	char *bar0;
	bar0 = (char*)pdx->map_bar0_addr;
	//return(map_bar0_addr[RegisterOffset/4]);
	return(ioread32(bar0+RegisterOffset));
}
//----------------------------------------------
static int Check_Busy(struct hws_pcie_dev *pdx)
{
	u32  statusreg;
	u32 timeout_loops = 0;
	const u32 max_timeout_loops = 200;
	//DbgPrint(("Check Busy in !!!\n"));
	//WRITE_REGISTER_ULONG((u32)(0x4000), 0x10);
	while (1)
	{
		statusreg = READ_REGISTER_ULONG(pdx,(u32)(CVBS_IN_BASE));
		if (statusreg == 0xFFFFFFFF)
		{
			break;
		}
		if ((statusreg & 0x08) == 0x00)
		{
			break;
		}
		timeout_loops++;
		if (timeout_loops >= max_timeout_loops) {
			pr_warn_ratelimited("hws: DSP busy wait timed out status=0x%x\n",
					    statusreg);
			return -ETIMEDOUT;
		}
		msleep(10);
	}
	//WRITE_REGISTER_ULONG((u32)(0x4000), 0x10);


	//DbgPrint(("Check Busy out !!!\n"));

	return 0;
}

static void StopDsp(struct hws_pcie_dev *pdx)
{
	//int j, i;
	u32  statusreg;
	statusreg = READ_REGISTER_ULONG(pdx,(u32)(CVBS_IN_BASE));
	if (statusreg == 0xFFFFFFFF)
	{
			return;
	}
	WRITE_REGISTER_ULONG(pdx,(u32)(CVBS_IN_BASE), 0x10);
	if (Check_Busy(pdx) < 0)
		pr_warn_ratelimited("hws: StopDsp did not reach idle state\n");
	WRITE_REGISTER_ULONG(pdx,( CVBS_IN_BASE + (2 * PCIE_BARADDROFSIZE)), 0x00);
		
	
}
static void EnableVideoCapture(struct hws_pcie_dev *pdx,int index,int en)
{
	ULONG status;
	int enable;
	if(pdx->m_PciDeviceLost) return;
	status = READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE+2*PCIE_BARADDROFSIZE));
	if(en)
	{
		enable =1;
		enable = enable<<index;
		status =  status|enable;
	}
	else
	{
		enable =1;
		enable = enable<<index;
		enable = ~enable;
		status =  status&enable;
	}
	pdx->m_bVCapStarted[index] = en;
	WRITE_REGISTER_ULONG(pdx,( CVBS_IN_BASE + (2 * PCIE_BARADDROFSIZE)), status);
	status = READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE+2*PCIE_BARADDROFSIZE));
	//printk("EnableVideoCapture[%d]=%X %d \n",index,status,pdx->m_bVCapStarted[index]);
}
static void EnableAudioCapture(struct hws_pcie_dev *pdx,int index,int en)
{
	ULONG status;
	int enable;
	if(pdx->m_PciDeviceLost) return;
	status =  READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE+3*PCIE_BARADDROFSIZE));

	if(en)
	{
		enable =1;
		enable = enable<<index;
		status =  status|enable;
	}
	else
	{
		enable =1;
		enable = enable<<index;
		enable = ~enable;
		status =  status&enable;
	}
	pdx->m_bACapStarted[index] = en;
	WRITE_REGISTER_ULONG(pdx,( CVBS_IN_BASE + (3 * PCIE_BARADDROFSIZE)), status);
	//printk("EnableAudioCapture =%X",status);
}

static int SetVideoFormteSize(struct hws_pcie_dev *pdx,int ch,int w,int h)
{
	   int hf_size;
	   int hf_size2;
	   //int frame_size;
		if(ch !=0) return -1;
		
		hf_size = (w*h*2)/(4*16*128);
		hf_size = hf_size*16*128;
		
		hf_size2 = (w*h*2)/(4*16*128);
		hf_size2 = w*h*2 -(hf_size2*16*128*3);
		//if((hf_size2 <0) ||(hf_size2 > m_MaxHWVideoBufferSize)||(hf_size >m_MaxHWVideoBufferSize))
		if(hf_size2 <0)
		{
			return -1;
		}
		pdx->m_format[0].dwWidth = w;		 // Image Width	
		pdx->m_format[1].dwWidth = w;		 // Image Width		
		pdx->m_format[2].dwWidth = w;		 // Image Width	
		pdx->m_format[3].dwWidth = w;		 // Image Width	
		
		pdx->m_format[0].dwHeight = h;	
		pdx->m_format[1].dwHeight = h;
		pdx->m_format[2].dwHeight = h;	
		pdx->m_format[3].dwHeight = h;
		
		pdx->m_format[0].HLAF_SIZE = hf_size; 
		pdx->m_format[1].HLAF_SIZE = hf_size; 
		pdx->m_format[2].HLAF_SIZE = hf_size;
		
		hf_size2 = hf_size2/(16*128);
		hf_size2 = (hf_size2+1)*16*128;
		pdx->m_format[3].HLAF_SIZE = hf_size2;
		return 1;
		
}

static void DmaMemFreePool(struct hws_pcie_dev *pdx)
	{
		//Trace t("DmaMemFreePool()");
		int	k;
		int index;
		unsigned long phyvirt_addr; 
		if(pdx->m_bBufferAllocate == TRUE)
		{
			//---------------
			for(index=0; index<pdx->m_nMaxChl; index++)
			{
				if(pdx->m_pbyVideoBuffer[index])
				{
					//printk("DmaMemFreePool ::m_pbyVideoBuffer = %p\n",  pdx->m_pbyVideoBuffer[index]);
					#if 0
					for (phyvirt_addr=(unsigned long)pdx->m_pbyVideoBuffer_area[i]; phyvirt_addr < ((unsigned long)pdx->m_pbyVideoBuffer_area[i] + pdx->m_MaxHWVideoBufferSize);phyvirt_addr+=PAGE_SIZE) 
					{
						// clear all pages
						ClearPageReserved(virt_to_page(phyvirt_addr));
					}
					kfree(pdx->m_pbyVideoBuffer[i]);
					#else
						dma_free_coherent(&pdx->pdev->dev, pdx->m_MaxHWVideoBufferSize, pdx->m_pbyVideoBuffer[index], pdx->m_pbyVideo_phys[index]);
					#endif 
					pdx->m_pbyVideoBuffer[index] = NULL;
				}				
			}
			//----------------------------------------------------
			for(index=0; index<pdx->m_nCurreMaxVideoChl;index++)
			{
				//printk("DmaMemFreePool ::m_pVideoScalerBuf = %p\n",  pdx->m_VideoInfo[index].m_pVideoScalerBuf);
				if(pdx->m_VideoInfo[index].m_pVideoScalerBuf !=NULL)
				{
					vfree(pdx->m_VideoInfo[index].m_pVideoScalerBuf);
					pdx->m_VideoInfo[index].m_pVideoScalerBuf = NULL;
				}
				for( k=0; k<MAX_VIDEO_QUEUE;k++)
				{
					//-------------------------
					//printk("DmaMemFreePool ::m_pVideoBufData[%d] = %p\n",k,pdx->m_VideoInfo[index].m_pVideoBufData[k]);
					if(pdx->m_VideoInfo[index].m_pVideoBufData[k])
					{
						for (phyvirt_addr=(unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area[k]; phyvirt_addr < ((unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area[k] + pdx->m_MaxHWVideoBufferSize);phyvirt_addr+=PAGE_SIZE) 
						{
								// clear all pages
							ClearPageReserved(virt_to_page(phyvirt_addr));
						}
						kfree(pdx->m_VideoInfo[index].m_pVideoBufData[k]);  
						pdx->m_VideoInfo[index].m_pVideoBufData[k] = NULL;
					}
					//---------------------------------
					//-------------------------
					//printk("DmaMemFreePool ::m_pVideoBufData1[%d] = %p\n",k,pdx->m_VideoInfo[index].m_pVideoBufData1[k]);
					if(pdx->m_VideoInfo[index].m_pVideoBufData1[k])
					{
						for (phyvirt_addr=(unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area1[k]; phyvirt_addr < ((unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area1[k] + pdx->m_MaxHWVideoBufferSize);phyvirt_addr+=PAGE_SIZE) 
						{
								// clear all pages
								ClearPageReserved(virt_to_page(phyvirt_addr));
						}
						kfree(pdx->m_VideoInfo[index].m_pVideoBufData1[k]);  
						pdx->m_VideoInfo[index].m_pVideoBufData1[k] = NULL;
					}
					//---------------------------------
					//-------------------------
					//printk("DmaMemFreePool ::m_pVideoBufData2[%d] = %p\n",k,pdx->m_VideoInfo[index].m_pVideoBufData2[k]);
					if(pdx->m_VideoInfo[index].m_pVideoBufData2[k])
					{
						for (phyvirt_addr=(unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area2[k]; phyvirt_addr < ((unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area2[k] + pdx->m_MaxHWVideoBufferSize);phyvirt_addr+=PAGE_SIZE) 
						{
								// clear all pages
								ClearPageReserved(virt_to_page(phyvirt_addr));
						}
						kfree(pdx->m_VideoInfo[index].m_pVideoBufData2[k]);  
						pdx->m_VideoInfo[index].m_pVideoBufData2[k] = NULL;
					}
					//---------------------------------
					//-------------------------
					//printk("DmaMemFreePool ::m_pVideoBufData3[%d] = %p\n",k,pdx->m_VideoInfo[index].m_pVideoBufData3[k]);
					if(pdx->m_VideoInfo[index].m_pVideoBufData3[k])
					{
						for (phyvirt_addr=(unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area3[k]; phyvirt_addr < ((unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area3[k] + pdx->m_MaxHWVideoBufferSize);phyvirt_addr+=PAGE_SIZE) 
						{
								// clear all pages
								ClearPageReserved(virt_to_page(phyvirt_addr));
						}
						kfree(pdx->m_VideoInfo[index].m_pVideoBufData3[k]);  
						pdx->m_VideoInfo[index].m_pVideoBufData3[k] = NULL;
					}
					//---------------------------------
					
				}
				//----audio release
				for( k=0; k<MAX_AUDIO_QUEUE;k++)
				{
					if(pdx->m_AudioInfo[index].m_pAudioBufData[k])
					{
						for (phyvirt_addr=(unsigned long)pdx->m_AudioInfo[index].m_pAudioData_area[k]; phyvirt_addr < ((unsigned long)pdx->m_AudioInfo[index].m_pAudioData_area[k] + pdx->m_dwAudioPTKSize);phyvirt_addr+=PAGE_SIZE) 
						{
								// clear all pages
								ClearPageReserved(virt_to_page(phyvirt_addr));
						}
						kfree(pdx->m_AudioInfo[index].m_pAudioBufData[k]);  
						pdx->m_AudioInfo[index].m_pAudioBufData[k] = NULL;
					}
				}
				
			}
			pdx->m_bBufferAllocate = FALSE;
		}
	}


static int  DmaMemAllocPool(struct hws_pcie_dev *pdx)
	{
		u32			status	= 0;
		uint8_t				i,k;
        dma_addr_t phy_addr;
		int  index;
		unsigned long phyvirt_addr; 
		if(pdx->m_bBufferAllocate == TRUE)
		{
			DmaMemFreePool(pdx);
		}
		//------------
		for(i=0; i<pdx->m_nMaxChl; i++)
		{
		
			//printk("kmalloc [%d]size=%X************\n", i,pdx->m_MaxHWVideoBufferSize);
			pdx->m_pbyVideoBuffer[i] = dma_alloc_coherent(&pdx->pdev->dev, pdx->m_MaxHWVideoBufferSize, &pdx->m_pbyVideo_phys[i], GFP_KERNEL);

			
			if(pdx->m_pbyVideoBuffer[i]== NULL)
			{
				printk("m_pbyVideoBuffer[%d] mem Allocate Fail ************\n", i);
				pdx->m_bBufferAllocate = TRUE;
				DmaMemFreePool(pdx);
				pdx->m_bBufferAllocate = FALSE;
				status = -1;
				return status;
			}
			#if 0
			pdx->m_pbyVideoBuffer_area[i] = (char *)(((unsigned long)pdx->m_pbyVideoBuffer[i] + PAGE_SIZE -1) & PAGE_MASK);
			for (phyvirt_addr=(unsigned long)pdx->m_pbyVideoBuffer_area[i]; phyvirt_addr < ((unsigned long)pdx->m_pbyVideoBuffer_area[i] + (pdx->m_MaxHWVideoBufferSize));
			phyvirt_addr+=PAGE_SIZE) 
			{
				// reserve all pages to make them remapable
				SetPageReserved(virt_to_page(phyvirt_addr));
			} 
			memset(pdx->m_pbyVideoBuffer[i] , 0x0,(pdx->m_MaxHWVideoBufferSize) );
			phy_addr= (dma_addr_t)virt_to_phys(pdx->m_pbyVideoBuffer[i]);
			pdx->m_pbyVideo_phys[i] = phy_addr;
			#else 
				phy_addr = 	pdx->m_pbyVideo_phys[i];
			#endif 
			//printk("PHY= %X=%X\n",phy_addr,pdx->m_pbyVideo_phys[i]);
			
			pdx->m_dwVideoBuffer[i] = 	  ((u64)phy_addr)&0xFFFFFFFF;
			pdx->m_dwVideoHighBuffer[i] = ((u64)phy_addr>>32)&0xFFFFFFFF;;

			pdx->m_pbyAudioBuffer[i] = (BYTE *)(pdx->m_pbyVideoBuffer[i] + pdx->m_MaxHWVideoBufferSize -MAX_AUDIO_CAP_SIZE );
			#if 0
				phy_addr= (dma_addr_t)virt_to_phys(pdx->m_pbyAudioBuffer[i]);
			#else
				phy_addr = pdx->m_pbyVideo_phys[i] + (pdx->m_MaxHWVideoBufferSize -MAX_AUDIO_CAP_SIZE);
			#endif 
			pdx->m_pbyAudio_phys[i] = phy_addr;
			
			pdx->m_dwAudioBuffer[i] =    pdx->m_dwVideoBuffer[i]+ pdx->m_MaxHWVideoBufferSize -MAX_AUDIO_CAP_SIZE;
			pdx->m_dwAudioBufferHigh[i] = pdx->m_dwVideoHighBuffer[i];
			//printk("[MV]Mem Video::m_dwVideoBuffer[%d] = %x\n", i, pdx->m_dwVideoBuffer[i]);
			//printk("[MV]Mem Video::m_dwVideoHighBuffer[%d] = %x\n", i, pdx->m_dwVideoHighBuffer[i]);
			//printk("[MV]Mem Audio::m_dwAudioBuffer[%d] = %x\n", i, pdx->m_dwAudioBuffer[i]);
			//printk("[MV]Mem Audio::m_dwAudioBufferHigh[%d] = %x\n", i, pdx->m_dwAudioBufferHigh[i]);

		}

		
         //KdPrint(("Mem allocate::m_dwAudioBuffer[%d] = %x\n", i, pdx->m_dwAudioBuffer));
		//-------------- video buffer 
		for(index=0; index<pdx->m_nCurreMaxVideoChl; index++)
		{
			pdx->m_VideoInfo[index].m_pVideoScalerBuf = vmalloc(MAX_VIDEO_HW_W*MAX_VIDEO_HW_H*2);
			if(pdx->m_VideoInfo[index].m_pVideoScalerBuf ==NULL)
			{
					pdx->m_bBufferAllocate = TRUE;
					DmaMemFreePool(pdx);
					pdx->m_bBufferAllocate = FALSE;
					status = -1;
					return status;
			}
			for( k=0; k<MAX_VIDEO_QUEUE;k++)
			{
				//----------------------------------- buf 
				pdx->m_VideoInfo[index].m_pVideoBufData[k]  = kmalloc((pdx->m_MaxHWVideoBufferSize), GFP_KERNEL);	
				if(!pdx->m_VideoInfo[index].m_pVideoBufData[k])
				{
					
					pdx->m_bBufferAllocate = TRUE;
					DmaMemFreePool(pdx);
					pdx->m_bBufferAllocate = FALSE;
					status = -1;
					return status;

				}
				else
				{

					pdx->m_VideoInfo[index].m_pVideoData_area[k] = (char *)(((unsigned long)pdx->m_VideoInfo[index].m_pVideoBufData[k] + PAGE_SIZE -1) & PAGE_MASK);
					for (phyvirt_addr=(unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area[k]; phyvirt_addr < ((unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area[k] + (pdx->m_MaxHWVideoBufferSize));
						phyvirt_addr+=PAGE_SIZE) 
					{
							// reserve all pages to make them remapable
							SetPageReserved(virt_to_page(phyvirt_addr));
					} 
					memset(pdx->m_VideoInfo[index].m_pVideoBufData[k] ,0x0,pdx->m_MaxHWVideoBufferSize );
				}
				//-------------------------------------------------------
				//----------------------------------- buf1 
				pdx->m_VideoInfo[index].m_pVideoBufData1[k]  = kmalloc((pdx->m_MaxHWVideoBufferSize), GFP_KERNEL);	
				if(!pdx->m_VideoInfo[index].m_pVideoBufData1[k])
				{
					
					pdx->m_bBufferAllocate = TRUE;
					DmaMemFreePool(pdx);
					pdx->m_bBufferAllocate = FALSE;
					status = -1;
					return status;

				}
				else
				{

					pdx->m_VideoInfo[index].m_pVideoData_area1[k] = (char *)(((unsigned long)pdx->m_VideoInfo[index].m_pVideoBufData[k] + PAGE_SIZE -1) & PAGE_MASK);
					for (phyvirt_addr=(unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area1[k]; phyvirt_addr < ((unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area1[k] + (pdx->m_MaxHWVideoBufferSize));
						phyvirt_addr+=PAGE_SIZE) 
					{
							// reserve all pages to make them remapable
							SetPageReserved(virt_to_page(phyvirt_addr));
					} 
					memset(pdx->m_VideoInfo[index].m_pVideoBufData1[k] ,0x0,pdx->m_MaxHWVideoBufferSize );
				}
				//-------------------------------------------------------
				//----------------------------------- buf2 
				pdx->m_VideoInfo[index].m_pVideoBufData2[k]  = kmalloc((pdx->m_MaxHWVideoBufferSize), GFP_KERNEL);	
				if(!pdx->m_VideoInfo[index].m_pVideoBufData2[k])
				{
					
					pdx->m_bBufferAllocate = TRUE;
					DmaMemFreePool(pdx);
					pdx->m_bBufferAllocate = FALSE;
					status = -1;
					return status;

				}
				else
				{

					pdx->m_VideoInfo[index].m_pVideoData_area2[k] = (char *)(((unsigned long)pdx->m_VideoInfo[index].m_pVideoBufData2[k] + PAGE_SIZE -1) & PAGE_MASK);
					for (phyvirt_addr=(unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area2[k]; phyvirt_addr < ((unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area2[k] + (pdx->m_MaxHWVideoBufferSize));
						phyvirt_addr+=PAGE_SIZE) 
					{
							// reserve all pages to make them remapable
							SetPageReserved(virt_to_page(phyvirt_addr));
					} 
					memset(pdx->m_VideoInfo[index].m_pVideoBufData2[k] ,0x0,pdx->m_MaxHWVideoBufferSize );
				}
				//-------------------------------------------------------
				//----------------------------------- buf3 
				pdx->m_VideoInfo[index].m_pVideoBufData3[k]  = kmalloc((pdx->m_MaxHWVideoBufferSize), GFP_KERNEL);	
				if(!pdx->m_VideoInfo[index].m_pVideoBufData3[k])
				{
					
					pdx->m_bBufferAllocate = TRUE;
					DmaMemFreePool(pdx);
					pdx->m_bBufferAllocate = FALSE;
					status = -1;
					return status;

				}
				else
				{

					pdx->m_VideoInfo[index].m_pVideoData_area3[k] = (char *)(((unsigned long)pdx->m_VideoInfo[index].m_pVideoBufData3[k] + PAGE_SIZE -1) & PAGE_MASK);
					for (phyvirt_addr=(unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area3[k]; phyvirt_addr < ((unsigned long)pdx->m_VideoInfo[index].m_pVideoData_area3[k] + (pdx->m_MaxHWVideoBufferSize));
						phyvirt_addr+=PAGE_SIZE) 
					{
							// reserve all pages to make them remapable
							SetPageReserved(virt_to_page(phyvirt_addr));
					} 
					memset(pdx->m_VideoInfo[index].m_pVideoBufData3[k] ,0x0,pdx->m_MaxHWVideoBufferSize );
				}
				//-------------------------------------------------------
				pdx->m_VideoInfo[index].pStatusInfo[k].byLock = MEM_UNLOCK;
				pdx->m_VideoInfo[index].m_nVideoIndex = 0;
			}
		}
		
	   //----------audio alloc 
	   #if 1
		for(i=0; i<pdx->m_nCurreMaxVideoChl; i++)
		{
			for( k=0; k<MAX_AUDIO_QUEUE;k++)
			{
				pdx->m_AudioInfo[i].m_pAudioBufData[k] =kmalloc(pdx->m_dwAudioPTKSize, GFP_KERNEL);
				if(!pdx->m_AudioInfo[i].m_pAudioBufData[k])
				{
					pdx->m_bBufferAllocate = TRUE;
					DmaMemFreePool(pdx);
					pdx->m_bBufferAllocate = FALSE;
		    		status = -1;
					return status;
				}
				else
				{
					pdx->m_AudioInfo[i].pStatusInfo[k].byLock = MEM_UNLOCK;
					pdx->m_AudioInfo[i].m_pAudioData_area[k] = (char *)(((unsigned long)pdx->m_AudioInfo[i].m_pAudioBufData[k] + PAGE_SIZE -1) & PAGE_MASK);
					for (phyvirt_addr=(unsigned long)pdx->m_AudioInfo[i].m_pAudioData_area[k]; phyvirt_addr < ((unsigned long)pdx->m_AudioInfo[i].m_pAudioData_area[k]  + pdx->m_dwAudioPTKSize);
							phyvirt_addr+=PAGE_SIZE) 
					{
						// reserve all pages to make them remapable
						SetPageReserved(virt_to_page(phyvirt_addr));
					} 
				}
			}
		}
	   #endif 
		//------------------------------------------------------------
		//KdPrint(("Mem allocate::m_pAudioData = %x\n",  pdx->m_pAudioData));
		pdx->m_bBufferAllocate = TRUE;
		//KdPrint(("DmaMemAllocPool  ed\n"));
		return 0;
}

static void StopDevice(struct hws_pcie_dev *pdx)
	{							// StopDevice		
		//Trace t("StopDevice()");
		int i;
		//int   device_lost =0;
		u32  statusreg;
        StopDsp(pdx);
		statusreg = READ_REGISTER_ULONG(pdx,(0x4000));
		//DbgPrint("[MV] Busy!!! statusreg =%X\n", statusreg);
		if (statusreg != 0xFFFFFFFF)
		{
			//set to one buffer mode 
	   	   //WRITE_REGISTER_ULONG((u32)(CVBS_IN_BASE + (25*PCIE_BARADDROFSIZE)), 0x00); //Buffer 1 address
		}
		else
		{
			pdx->m_PciDeviceLost = 1;
		}
		pdx->m_bStartRun = 0;
		if(pdx->m_PciDeviceLost ==0)
		{
			for (i = 0; i<MAX_VID_CHANNELS; i++)
			{
				EnableVideoCapture(pdx,i,0);
				EnableAudioCapture(pdx,i,0);
			}
		}
		//if(device_lost) return;
		DmaMemFreePool(pdx);		 
		//printk("StopDevice Done\n");

		
	}	
static void irq_teardown(struct hws_pcie_dev *lro)
{
	//int i;

	//BUG_ON(!lro);

	//if (lro->msix_enabled) {
	//	for (i = 0; i < lro->irq_user_count; i++) {
	//		printk("Releasing IRQ#%d\n", lro->entry[i].vector);
	//		free_irq(lro->entry[i].vector, &lro->user_irq[i]);
	//	}
	//} 
	//else 

	if (lro->irq_line != -1) {
		//printk("Releasing IRQ#%d\n", lro->irq_line);
		free_irq(lro->irq_line, lro);
	}
}
static void StopKSThread(struct hws_pcie_dev *pdx)
{
	if(pdx->mMain_tsk)
	{
		kthread_stop(pdx->mMain_tsk);	
	}
}

//----------------------------
static void hws_remove(struct pci_dev *pdev)
{
	int i;
	struct video_device *vdev;
	struct hws_pcie_dev *dev = 
		(struct hws_pcie_dev*) pci_get_drvdata(pdev);
	//----------------------------
	if(dev->map_bar0_addr == NULL) return;
	hws_diag_remove_debugfs();
	//StopSys(dev);
	StopDevice(dev);
	/* disable interrupts */
	irq_teardown(dev);
	StopKSThread(dev);
	//printk("hws_remove  0\n");
	for ( i = 0; i<dev->m_nCurreMaxVideoChl; i++)
	{
		tasklet_kill(&dev->dpc_video_tasklet[i]);
		tasklet_kill(&dev->dpc_audio_tasklet[i]);	
	}
	//-------------------------
	//printk("hws_remove  1\n");
	for(i=0;i<dev->m_nCurreMaxVideoChl;i++){
		if(dev->audio[i].resampled_buf)
		{
			vfree(dev->audio[i].resampled_buf);
			dev->audio[i].resampled_buf = NULL;
		}
		if(dev->audio[i].card)
		{
			snd_card_free(dev->audio[i].card);
			dev->audio[i].card=NULL;
		}
	}	
	for(i=0;i<dev->m_nCurreMaxVideoChl;i++){
		vdev = &dev->video[i].vdev;
		video_unregister_device(vdev);
		v4l2_device_unregister(&dev->video[i].v4l2_dev);
	}
	//-----------------
	if(dev->wq)
	{
		destroy_workqueue(dev->wq);
	}
	
	if(dev->auwq)
	{
		for (i = 0; i < dev->m_nCurreMaxVideoChl; i++)
			cancel_delayed_work_sync(&dev->audio[i].silence_work);
		destroy_workqueue(dev->auwq);
	}
	dev->wq=NULL;
	dev->auwq=NULL;
    //free_irq(dev->pdev->irq, dev);

	iounmap(dev->info.mem[0].internal_addr);
	
	//pci_disable_device(pdev);
	if (dev->msix_enabled) 
	{		
			pci_disable_msix(pdev); 	
			dev->msix_enabled = 0; 
	}	
	else if (dev->msi_enabled)
	{
			pci_disable_msi(pdev);		
			dev->msi_enabled = 0;	
	}
	kfree(dev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	//printk("hws_remove  Done\n");
}
//---------------------------------------	
static void CheckCardStatus(struct hws_pcie_dev *pdx)
{
	ULONG status;
	status = READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE+0*PCIE_BARADDROFSIZE));
	//DbgPrint("CheckCardStatus =%X",status);
	if((status&0x01) != 0x01)
	{
		//DbgPrint("CheckCardStatus =%X",status);
		InitVideoSys(pdx,1);
	}
	
}
static int CheckVideoCapture(struct hws_pcie_dev *pdx,int index)
{
	ULONG status;
	int enable;
	status = READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE+2*PCIE_BARADDROFSIZE));
	enable = (status >>index)&0x01;
	return enable;
}
static int CheckAudioCapture(struct hws_pcie_dev *pdx,int index)
{
	ULONG status;
	int enable;
	status = READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE+3*PCIE_BARADDROFSIZE));
	enable = (status >>index)&0x01;
	return enable;
}

static int  StartAudioCapture(struct hws_pcie_dev *pdx,int index)
{
	int j;
	
	if(pdx->m_bACapStarted[index]==1) 
	{
		if(CheckAudioCapture(pdx,index) ==0)
		{
			CheckCardStatus(pdx);
			EnableAudioCapture(pdx,index,1);
		}
		//DbgPrint("Re StartAudioCapture =%d",index);	
		return -1;
	}
	CheckCardStatus(pdx);
	pdx->m_bAudioRun[index] = 1;
	pdx->m_bAudioStop[index] = 0;
	pdx->m_nAudioBufferIndex[index] =0; 
	pdx->audio_data[index]=0;
	pdx->m_nRDAudioIndex[index] =0;
	WRITE_ONCE(pdx->audio[index].last_irq_ns, 0);
	WRITE_ONCE(pdx->audio[index].last_copy_ns, 0);
	WRITE_ONCE(pdx->audio[index].last_progress_ns, ktime_get_ns());
	for(j=0; j<MAX_AUDIO_QUEUE;j++)
	{
		pdx->m_AudioInfo[index].pStatusInfo[j].byLock = MEM_UNLOCK;
	}
	pdx->m_AudioInfo[index].dwisRuning =1;
	EnableAudioCapture(pdx,index,1);
	return 0;
}

static int StartVideoCapture(struct hws_pcie_dev *pdx,int index)
{
	int j;
	//unsigned long flags;
	if(pdx->m_bVCapStarted[index]==1) 
	{

		CheckCardStatus(pdx);
		if(CheckVideoCapture(pdx,index) ==0)
		{
			EnableVideoCapture(pdx,index,1);
		}
		return -1;
	}
	//--------------------
	CheckCardStatus(pdx);
	//--------------------
	//spin_lock_irqsave(&pdx->videoslock[index], flags);
	for (j = 0; j<MAX_VIDEO_QUEUE; j++)
	{
			pdx->m_pVCAPStatus[index][j].byLock = MEM_UNLOCK;
			pdx->m_pVCAPStatus[index][j].byPath = 2;
			
	}
	//spin_unlock_irqrestore(&pdx->videoslock[index], flags);
	//pdx->m_nVideoIndex[index] =0;
	pdx->m_VideoInfo[index].dwisRuning = 1;
	pdx->m_VideoInfo[index].m_nVideoIndex =0;
	pdx->m_nRDVideoIndex[index]=0;
	pdx->m_bChangeVideoSize[index] = 0;
	pdx->m_bVCapIntDone[index] = 1;
	pdx->m_pVideoEvent[index] = 1;
	pdx->m_nVideoBusy[index] =0;
	pdx->video_data[index]  =0;
	EnableVideoCapture(pdx,index,1);
	return 0;
}

static void StopVideoCapture(struct hws_pcie_dev *pdx,int index)
{
	//int inc=0;
	
	if(pdx->m_bVCapStarted[index] ==0) return;	
	//pdx->m_nVideoIndex[index] =0;
	pdx->m_VideoInfo[index].dwisRuning = 0;
	pdx->m_bVideoStop[index] = 1;
	pdx->m_pVideoEvent[index] = 0;
	pdx->m_bChangeVideoSize[index] = 0;
	#if 0
	while(1)
	{
		if(pdx->m_bVideoStop[index] ==0)
		{
			break;
		}
		inc++;
		if(inc >2000)
		{
			break;
		}
		msleep(10);
	}
	#endif 
	EnableVideoCapture(pdx,index,0);
	pdx->m_bVCapIntDone[index] = 0;
}
static void StopAudioCapture(struct hws_pcie_dev *pdx,int index)
{
	//int inc=0;
	if(pdx->m_bAudioRun[index] ==0) return;
	pdx->m_bAudioRun[index] = 0;
	pdx->m_bAudioStop[index] = 1;
	pdx->m_nAudioBufferIndex[index] =0;
	pdx->m_AudioInfo[index].dwisRuning =0;
	cancel_delayed_work(&pdx->audio[index].silence_work);
	WRITE_ONCE(pdx->audio[index].last_irq_ns, 0);
	WRITE_ONCE(pdx->audio[index].last_copy_ns, 0);
	WRITE_ONCE(pdx->audio[index].last_progress_ns, 0);
	#if 0
	while(1)
	{
		if(pdx->m_bAudioStop[index] ==0)
		{
			break;
		}
		inc++;
		if(inc >2000)
		{
			break;
		}
		msleep(10);
	}
	#endif 
	EnableAudioCapture(pdx,index,0);
}
//-----------------------------


//-----------------------------
static int MemCopyVideoToSteam(struct hws_pcie_dev *pdx,int nDecoder)
	{
		int nIndex = -1;
		//int i=0 ;
		int status =-1;
		BYTE *bBuf = NULL;
		BYTE *bBuf1 = NULL;
		BYTE *pSrcBuf= NULL;
		BYTE *pDmaSrcBuf= NULL;
		BYTE *pSrcBuf1= NULL;
		BYTE *pDmaSrcBuf1= NULL;
		int dwSrcPitch;
		//int dwMaskPitch;
		int copysize;
		int copysize1;
		int nw,nh;
		int interlace;
		int mVideoBufIndex;
		//int halfsize;
		int *pMask;
		int  line_cnt=0;
		unsigned long flags;

		//---------------------
		nw = pdx->m_pVCAPStatus[nDecoder][0].dwWidth ;
		nh = pdx->m_pVCAPStatus[nDecoder][0].dwHeight;
		if((nw <0)||(nh<0)||(nw>MAX_VIDEO_HW_W)||(nh>MAX_VIDEO_HW_H))
		{
			return -1;
		}
		mVideoBufIndex = pdx->m_nVideoBufferIndex[nDecoder]; 
		interlace  = pdx->m_pVCAPStatus[nDecoder][0].dwinterlace;
		if(pdx->m_Device_SupportYV12 ==1)
		{
			dwSrcPitch = nw*12/8;
		}
		else
		{
			dwSrcPitch = nw*2;
		}
		if(mVideoBufIndex== 1)
		{
						
			pDmaSrcBuf = pdx->m_pbyVideoBuffer[0];
			pDmaSrcBuf1 = pdx->m_pbyVideoBuffer[1];
			//pci_dma_sync_single_for_cpu(pdx->pdev,pdx->m_pbyVideo_phys[0],pdx->m_MaxHWVideoBufferSize,2);
			//pci_dma_sync_single_for_cpu(pdx->pdev,pdx->m_pbyVideo_phys[1],pdx->m_MaxHWVideoBufferSize,2);
			dma_sync_single_for_cpu(&pdx->pdev->dev,pdx->m_pbyVideo_phys[0],pdx->m_MaxHWVideoBufferSize,2);
			dma_sync_single_for_cpu(&pdx->pdev->dev,pdx->m_pbyVideo_phys[1],pdx->m_MaxHWVideoBufferSize,2);
			
				
			copysize = pdx->m_format[0].HLAF_SIZE;
			copysize1 = pdx->m_format[1].HLAF_SIZE;
			line_cnt = copysize1/dwSrcPitch;
		}
		else
		{
			pDmaSrcBuf = pdx->m_pbyVideoBuffer[2];
			pDmaSrcBuf1 = pdx->m_pbyVideoBuffer[3];
			//pci_dma_sync_single_for_cpu(pdx->pdev,pdx->m_pbyVideo_phys[2],pdx->m_MaxHWVideoBufferSize,2);
			//pci_dma_sync_single_for_cpu(pdx->pdev,pdx->m_pbyVideo_phys[3],pdx->m_MaxHWVideoBufferSize,2);
			dma_sync_single_for_cpu(&pdx->pdev->dev,pdx->m_pbyVideo_phys[2],pdx->m_MaxHWVideoBufferSize,2);
			dma_sync_single_for_cpu(&pdx->pdev->dev,pdx->m_pbyVideo_phys[3],pdx->m_MaxHWVideoBufferSize,2);
			copysize = pdx->m_format[2].HLAF_SIZE;
			copysize1 = pdx->m_format[3].HLAF_SIZE;
			line_cnt = copysize1/dwSrcPitch;

		}
		pMask = (int*)(pDmaSrcBuf1+(copysize1-(line_cnt-1)*dwSrcPitch));
		if(*pMask == 0x55AAAA55)
		{
			//DbgPrint("########-*pMask- [%d]%X[%d-%d]\n",nDecoder, *pMask,nw,nh);
			//------------------------------
			if(pdx->m_nVideoHalfDone[nDecoder] == 1)
			{
				pdx->m_nVideoHalfDone[nDecoder] =0; 
			}
			//------------------------------
			return -1;
		}
		else					
		{							
			if(mVideoBufIndex== 0)							
			{								
				if(pdx->m_nVideoHalfDone[nDecoder]  ==0)
				{									
						//DbgPrint("X1:HLAF ########-*pMask- [%d] [%d]\n",nDecoder,mVideoBufIndex);										
						*pMask = 0x55AAAA55;
						return -1;								
				}
				else
				{
					pdx->m_nVideoHalfDone[nDecoder] =0; 
				}
			}
		}		
		//-------------------------------
		nIndex = -1;
		pSrcBuf = pDmaSrcBuf;
		pSrcBuf1 = pDmaSrcBuf1;
		bBuf =NULL;
		bBuf1 = NULL;
		if(pdx->m_VideoInfo[nDecoder].dwisRuning ==1)
		{
			//--------------			
			if(mVideoBufIndex== 1)
			{
				if(pdx->m_VideoInfo[nDecoder].pStatusInfo[pdx->m_VideoInfo[nDecoder].m_nVideoIndex].byLock== MEM_UNLOCK)
	
				{
						nIndex = pdx->m_VideoInfo[nDecoder].m_nVideoIndex;
						bBuf =	pdx->m_VideoInfo[nDecoder].m_pVideoBufData[nIndex];
						bBuf1 = pdx->m_VideoInfo[nDecoder].m_pVideoBufData1[nIndex];

				}
			}
			else
			{
					   nIndex = pdx->m_VideoInfo[nDecoder].m_nVideoIndex;
					   bBuf =  pdx->m_VideoInfo[nDecoder].m_pVideoBufData2[nIndex];
					   bBuf1 = pdx->m_VideoInfo[nDecoder].m_pVideoBufData3[nIndex];

			}
			if(nIndex== -1)
			{
							
					//if(pdx->m_VideoInfo[nDecoder].pStatusInfo[video_index][pdx->m_VideoInfo[nDecoder].m_nVideoIndex[video_index]].byField== 0)
				//{
					pdx->m_VideoInfo[nDecoder].pStatusInfo[pdx->m_VideoInfo[nDecoder].m_nVideoIndex].byLock= MEM_UNLOCK;
					nIndex = pdx->m_VideoInfo[nDecoder].m_nVideoIndex;
					bBuf =  pdx->m_VideoInfo[nDecoder].m_pVideoBufData[nIndex];
					bBuf1 = pdx->m_VideoInfo[nDecoder].m_pVideoBufData1[nIndex];
				//}
			}

			//-------------------
			if((nIndex!= -1)&& bBuf&&bBuf1)
			{
				memcpy(bBuf,pSrcBuf,copysize);
				memcpy(bBuf1,pSrcBuf1,copysize1);
				//----------------------
					if(mVideoBufIndex== 0)
					{
							status = 0;
							spin_lock_irqsave(&pdx->videoslock[nDecoder], flags);
							
							pdx->m_VideoInfo[nDecoder].m_nVideoIndex = nIndex+1;
							if(pdx->m_VideoInfo[nDecoder].m_nVideoIndex >= MAX_VIDEO_QUEUE)
							{
								pdx->m_VideoInfo[nDecoder].m_nVideoIndex =0;
							}
								pdx->m_VideoInfo[nDecoder].m_VideoBufferSize[2] = copysize;
								pdx->m_VideoInfo[nDecoder].m_VideoBufferSize[3] = copysize1;
								pdx->m_VideoInfo[nDecoder].pStatusInfo[nIndex].dwWidth = pdx->m_pVCAPStatus[nDecoder][0].dwWidth ; 
								pdx->m_VideoInfo[nDecoder].pStatusInfo[nIndex].dwHeight = pdx->m_pVCAPStatus[nDecoder][0].dwHeight;
								pdx->m_VideoInfo[nDecoder].pStatusInfo[nIndex].dwinterlace = interlace;
								pdx->m_VideoInfo[nDecoder].pStatusInfo[nIndex].byLock = MEM_LOCK;

							spin_unlock_irqrestore(&pdx->videoslock[nDecoder], flags);
					}
					else
					{
						pdx->m_VideoInfo[nDecoder].m_VideoBufferSize[0] = copysize;
						pdx->m_VideoInfo[nDecoder].m_VideoBufferSize[1] = copysize1;
						pdx->m_nVideoHalfDone[nDecoder] = 1; 

					}
					 
				}
				else
				{
					 //printk("No Buffer Write %d",nDecoder);
					 //queue_work(pdx->wq,&pdx->video[nDecoder].videowork);
					 pdx->m_nVideoHalfDone[nDecoder] =0; 
				}
		}
		*pMask = 0x55AAAA55;
		return status;
}
static int SetQuene(struct hws_pcie_dev  *pdx,int nDecoder)
	{
		int status =-1;
		//KLOCK_QUEUE_HANDLE  oldirql;
		//DbgPrint("SetQuene %d %d",nDecoder,pdx->m_bStartRun);
		if(!pdx->m_bStartRun)
		{
		  return -1 ;
		}
		//DbgPrint("SetQuene 2 %d %d",nDecoder,pdx->m_bRun[nDecoder]);
		if(!pdx->m_bVCapStarted[nDecoder])
		{
		  	if(pdx->m_bVideoStop[nDecoder] == 1)
		  	{
				pdx->m_bVideoStop[nDecoder] =0;
				//KeSetEvent(& pdx->m_pVideoExitEvent[nDecoder],IO_NO_INCREMENT,FALSE); 
				//DbgPrint("KeSetEvent Exit Event[%d]\n",nDecoder);
			}
		  
		  return -1 ;
		}
		pdx->m_nVideoBusy[nDecoder] = 1; 
		//-------------------------------
		//DbgPrint("SetQuene 3 %d %d",nDecoder,pdx->m_bVCapStarted[nDecoder]);
		if(pdx->m_bVCapStarted[nDecoder] == TRUE)
		{
			status = MemCopyVideoToSteam(pdx,nDecoder);	
		}
		pdx->m_nVideoBusy[nDecoder] = 0;
		return status;
}

//------------------------------------
static int MemCopyAudioToSteam( struct hws_pcie_dev  *pdx,int dwAudioCh)
{
	int i=0;
	bool diag = hws_diag_enabled();
	BYTE *bBuf = NULL;
	BYTE *pSrcBuf= NULL;
	int nIndex = -1;
	int status = 0;
	unsigned long flags;
	u32 free_slots = 0;
	u64 now_ns = ktime_get_ns();
	u64 last_irq_ns;
	if (dwAudioCh < 0 || dwAudioCh >= MAX_VID_CHANNELS)
		return -EINVAL;

	if (pdx->m_dwAudioPTKSize <= 0 || pdx->m_dwAudioPTKSize > MAX_AUDIO_CAP_SIZE) {
		if (diag)
			atomic64_inc(&hws_audio_diag[dwAudioCh].bad_packet_sizes);
		if (hws_audio_trace_enabled())
			trace_printk("hws_audio_drop ch=%d reason=bad_packet ptk=%u\n", dwAudioCh, pdx->m_dwAudioPTKSize);
		return -EINVAL;
	}
	//printk("MemCopyAudioToSteam =%d",dwAudioCh);
	if(pdx->m_nAudioBufferIndex[dwAudioCh]== 0)
	{
							
		pSrcBuf = pdx->m_pbyAudioBuffer[dwAudioCh]+pdx->m_dwAudioPTKSize;
	}
	else
	{
		pSrcBuf =  pdx->m_pbyAudioBuffer[dwAudioCh];
	}
	
	//-----------------------------------------------------
		nIndex = -1;
		if(pdx->m_AudioInfo[dwAudioCh].dwisRuning ==1)
		{
			for( i = pdx->m_AudioInfo[dwAudioCh].m_nAudioIndex;i<MAX_AUDIO_QUEUE;i++)
			{
				if(pdx->m_AudioInfo[dwAudioCh].pStatusInfo[i].byLock== MEM_UNLOCK)
				{
						nIndex =i;
						bBuf = pdx->m_AudioInfo[dwAudioCh].m_pAudioBufData[i];
						break;
				}
			}
			if(nIndex == -1)
			{
				for( i = 0 ;i<pdx->m_AudioInfo[dwAudioCh].m_nAudioIndex;i++)
				{
					if(pdx->m_AudioInfo[dwAudioCh].pStatusInfo[i].byLock== MEM_UNLOCK)
					{
						nIndex =i;
						bBuf = pdx->m_AudioInfo[dwAudioCh].m_pAudioBufData[i];
						break;
					}
				
				}
			}
			
			if (diag) {
				free_slots = 0;
				for (i = 0; i < MAX_AUDIO_QUEUE; i++) {
					if (pdx->m_AudioInfo[dwAudioCh].pStatusInfo[i].byLock == MEM_UNLOCK)
						free_slots++;
				}
				hws_audio_diag_record_queue_free(dwAudioCh, free_slots);
			}

			if((nIndex!= -1)&& bBuf)
			{

					//pci_dma_sync_single_for_cpu(pdx->pdev,pdx->m_pbyAudio_phys[dwAudioCh],MAX_AUDIO_CAP_SIZE,2);
					dma_sync_single_for_cpu(&pdx->pdev->dev,pdx->m_pbyAudio_phys[dwAudioCh],MAX_AUDIO_CAP_SIZE,2);
					memcpy(bBuf, pSrcBuf, pdx->m_dwAudioPTKSize);
					
					pdx->m_AudioInfo[dwAudioCh].m_nAudioIndex = nIndex+1;
					if(pdx->m_AudioInfo[dwAudioCh].m_nAudioIndex>= MAX_AUDIO_QUEUE)
					{
						pdx->m_AudioInfo[dwAudioCh].m_nAudioIndex =0;
					}
					spin_lock_irqsave(&pdx->audiolock[dwAudioCh], flags);
					pdx->m_AudioInfo[dwAudioCh].pStatusInfo[nIndex].dwLength = pdx->m_dwAudioPTKSize ;
			 		pdx->m_AudioInfo[dwAudioCh].pStatusInfo[nIndex].byLock = MEM_LOCK;
					spin_unlock_irqrestore(&pdx->audiolock[dwAudioCh], flags);
					//KeSetEvent(& pdx->m_AudioInfo[dwAudioCh].m_pAudioEvent[audio_index],IO_NO_INCREMENT,FALSE); 
					//printk("Set Audio Event %d\n",dwAudioCh);
					//pdx->audio[dwAudioCh].pos = pdx->m_dwAudioPTKSize;
					 //snd_pcm_period_elapsed(pdx->audio[dwAudioCh].substream);	
					 last_irq_ns = READ_ONCE(pdx->audio[dwAudioCh].last_irq_ns);
					 if (diag && last_irq_ns && now_ns >= last_irq_ns)
						hws_audio_diag_record_latency(dwAudioCh, &hws_audio_diag[dwAudioCh].irq_to_copy_ns_total,
									 &hws_audio_diag[dwAudioCh].irq_to_copy_ns_max,
									 &hws_audio_diag[dwAudioCh].irq_to_copy_samples,
									 now_ns - last_irq_ns);
					 WRITE_ONCE(pdx->audio[dwAudioCh].last_copy_ns, now_ns);
					 if (!queue_work(pdx->auwq,&pdx->audio[dwAudioCh].audiowork)) {
					if (diag)
						atomic64_inc(&hws_audio_diag[dwAudioCh].workqueue_requeues);
				 }
					//pdx->m_AudioInfo[dwAudioCh].pStatusInfo[nIndex].byLock = MEM_UNLOCK;		
				
			}
			else
			{
				if (diag) {
					atomic64_inc(&hws_audio_diag[dwAudioCh].no_free_queue_slots);
					atomic64_inc(&hws_audio_diag[dwAudioCh].memcopy_failures);
				}
				pr_info_ratelimited("hws: no audio buffer ch=%d\n", dwAudioCh);
				if (hws_audio_trace_enabled())
					trace_printk("hws_audio_drop ch=%d reason=no_free_queue ptk=%u\n", dwAudioCh, pdx->m_dwAudioPTKSize);
				return -ENOSPC;

			}
			return status;
	}
	if (diag) {
		atomic64_inc(&hws_audio_diag[dwAudioCh].memcopy_failures);
		atomic64_inc(&hws_audio_diag[dwAudioCh].stream_not_running);
	}
	if (hws_audio_trace_enabled())
		trace_printk("hws_audio_drop ch=%d reason=stream_not_running\n", dwAudioCh);
	return -EAGAIN;
}

static int SetAudioQuene( struct hws_pcie_dev *pdx,int dwAudioCh)
{
	int status =-1;
	//int i;
	//BYTE *bBuf = NULL;
	//BYTE *pSrcBuf= NULL;
	//int nIndex = -1;
	//printk("SetAudioQuene =%d",dwAudioCh);
	if (dwAudioCh < 0 || dwAudioCh >= MAX_VID_CHANNELS)
		return -EINVAL;

	if(!pdx->m_bACapStarted[dwAudioCh])
	{
		  return -1 ;
	}
	if (READ_ONCE(pdx->m_curr_No_Video[dwAudioCh])) {
		int i;
		unsigned long flags;

		spin_lock_irqsave(&pdx->audiolock[dwAudioCh], flags);
		for (i = 0; i < MAX_AUDIO_QUEUE; i++) {
			if (pdx->m_AudioInfo[dwAudioCh].pStatusInfo[i].byLock == MEM_LOCK)
				pdx->m_AudioInfo[dwAudioCh].pStatusInfo[i].byLock = MEM_UNLOCK;
		}
		pdx->m_nRDAudioIndex[dwAudioCh] = 0;
		spin_unlock_irqrestore(&pdx->audiolock[dwAudioCh], flags);

		hws_inject_silence_packet(pdx, dwAudioCh, pdx->m_dwAudioPTKSize,
					  HWS_AUDIO_SILENCE_NO_VIDEO);

		pdx->m_nAudioBusy[dwAudioCh] = 0;
		return 0;
	}
	if(!pdx->m_bAudioRun[dwAudioCh])
	{
				if(pdx->m_bAudioStop[dwAudioCh] == 1)
				{
					pdx->m_bAudioStop[dwAudioCh] =0;
					//DbgPrint("DpcForIsr_Audio0 Exit Event[%d]\n",dwAudioCh);
				}
				pdx->m_nAudioBusy[dwAudioCh] =0;
				return status;
	}
	
	pdx->m_nAudioBusy[dwAudioCh]  = 1;

	status = MemCopyAudioToSteam(pdx,dwAudioCh);
	if (status < 0) {
		/* Keep PCM timing continuous on unstable/missing capture packets. */
		if (hws_audio_trace_enabled())
			trace_printk("hws_audio_drop ch=%d reason=memcopy_fail err=%d ptk=%u\n",
				     dwAudioCh, status, pdx->m_dwAudioPTKSize);
		hws_inject_silence_packet(pdx, dwAudioCh, pdx->m_dwAudioPTKSize,
					  HWS_AUDIO_SILENCE_FALLBACK);
		status = 0;
	}

	pdx->m_nAudioBusy[dwAudioCh] = 0;	


	return status;

}

static void DpcForIsr_Audio0(unsigned long data)
{
	    
		int index;
	 	struct hws_pcie_dev *pdx;
		//pdx = sys_dvrs_hw_pdx;
		pdx = (struct hws_pcie_dev *)data;
		//unsigned long *pdata = (unsigned long *)data;
		//curr_buf_index = *pdata;
		index =0;
		SetAudioQuene(pdx,index);
		
}
#if 0
static void DpcForIsr_Audio1(unsigned long data)
{
	    
		int index;
	 	struct hws_pcie_dev *pdx;
		//pdx = sys_dvrs_hw_pdx;
		pdx = (struct hws_pcie_dev *)data;
		//unsigned long *pdata = (unsigned long *)data;
		//curr_buf_index = *pdata;
		index =1;
		SetAudioQuene(pdx,index);
		
}
static void DpcForIsr_Audio2(unsigned long data)
{
	    
		int index;
	 	struct hws_pcie_dev *pdx;
		//pdx = sys_dvrs_hw_pdx;
		pdx = (struct hws_pcie_dev *)data;
		//unsigned long *pdata = (unsigned long *)data;
		//curr_buf_index = *pdata;
		index =2;
		SetAudioQuene(pdx,index);
		
}
static void DpcForIsr_Audio3(unsigned long data)
{
	    
		int index;
	 	struct hws_pcie_dev *pdx;
		//pdx = sys_dvrs_hw_pdx;
		pdx = (struct hws_pcie_dev *)data;
		//unsigned long *pdata = (unsigned long *)data;
		//curr_buf_index = *pdata;
		index =3;
		SetAudioQuene(pdx,index);
}
#endif 

static void DpcForIsr_Video0(unsigned long data)
	{
		int i = 0;
	    int ret;
		//int curr_buf_index;
	 	struct hws_pcie_dev *pdx;
		//pdx = sys_dvrs_hw_pdx;
		pdx = (struct hws_pcie_dev *)data;
		//unsigned long *pdata = (unsigned long *)data;
		//curr_buf_index = *pdata;
		//printk("DpcForIsr_Video0\n");
		ret = SetQuene(pdx,i);
		//printk("[%X] pdx->m_bVCapStarted[i]=%d  ret=%d\n", pdx->pdev->device,pdx->m_bVCapStarted[i],ret);
		if(ret != 0 )
		{
			return;

		}
		
		if(pdx->m_bVCapStarted[i] == TRUE)
		{
			//printk("pdx->m_bVCapIntDone[i] = %d\n", pdx->m_bVCapIntDone[i]);
			//printk("pdx->m_pVideoEvent[i] = %d\n", pdx->m_pVideoEvent[i]);
			
			if((pdx->m_bVCapIntDone[i] == TRUE) && pdx->m_pVideoEvent[i])
			{
				pdx->m_bVCapIntDone[i] = FALSE;
				//printk("pdx->m_bChangeVideoSize[i] = %d\n",pdx->m_bChangeVideoSize[i]);
				if((!pdx->m_bChangeVideoSize[i])&&(pdx->m_pVideoEvent[i])) 
				{
					
					 //pdx->wq_flag[i] = 1;
					 //wake_up_interruptible(&pdx->wq_video[i]);  
					 //printk("Set Event\n");
					 queue_work(pdx->wq,&pdx->video[i].videowork);
				}
				else
				{
					 pdx->m_bChangeVideoSize[i] = 0;
				}
				
			}
		}
	}
	#if 0
	static void DpcForIsr_Video1(unsigned long data)
	{
	
		int i = 1;
		int ret;
	    //int curr_buf_index;
	   struct hws_pcie_dev *pdx;
		pdx = (struct hws_pcie_dev *)data;
		//pdx = sys_dvrs_hw_pdx;
		
		//unsigned long *pdata = (unsigned long *)data;
		//curr_buf_index = *pdata;
		
		ret = SetQuene(pdx,i);
		if(ret != 0 ) 
		{
		
			return;
		}
	
		if(pdx->m_bVCapStarted[i] == TRUE)
		{
			
			if(pdx->m_bVCapIntDone[i] == TRUE && pdx->m_pVideoEvent[i])
			{
                pdx->m_bVCapIntDone[i] = FALSE;
				if(!pdx->m_bChangeVideoSize[i]) 
				{
					if((!pdx->m_bChangeVideoSize[i])&&(pdx->m_pVideoEvent[i])) 
					{
					  //pdx->wq_flag[i] = 1;
					  //wake_up_interruptible(&pdx->wq_video[i]); 
					   queue_work(pdx->wq,&pdx->video[i].videowork);
					}
				}
				else
				{
					pdx->m_bChangeVideoSize[i] = 0;
				}
			}
		}
	}
	
	static void DpcForIsr_Video2(unsigned long data)
	{
	
		int i = 2;
		int ret;
		//int curr_buf_index;
		struct hws_pcie_dev *pdx;
		//pdx = sys_dvrs_hw_pdx;
		pdx = (struct hws_pcie_dev *)data;
		//unsigned long *pdata = (unsigned long *)data;
		//curr_buf_index = *pdata;
		 ret = SetQuene(pdx,i);
		if(ret != 0 ) 
		{
			return;
		}
		
		if(pdx->m_bVCapStarted[i] == TRUE)
		{
			
			if(pdx->m_bVCapIntDone[i] == TRUE && pdx->m_pVideoEvent[i])
			{
                pdx->m_bVCapIntDone[i] = FALSE;
				if(!pdx->m_bChangeVideoSize[i]) 
				{
					if((!pdx->m_bChangeVideoSize[i])&&(pdx->m_pVideoEvent[i])) 
					{
					   //pdx->wq_flag[i] = 1;
					   //wake_up_interruptible(&pdx->wq_video[i]); 
					    queue_work(pdx->wq,&pdx->video[i].videowork);
					}
				}
				else
				{
					pdx->m_bChangeVideoSize[i] = 0;
				}
			}
		}

	}
	
	static void DpcForIsr_Video3(unsigned long data)
	{
	
		int i = 3;
		int ret;
		//int curr_buf_index;
		struct hws_pcie_dev *pdx;
		//pdx = sys_dvrs_hw_pdx;
		pdx = (struct hws_pcie_dev *)data;
		//unsigned long *pdata = (unsigned long *)data;
		//curr_buf_index = *pdata;
		//mutex_lock(&pdx->video_mutex[i]); 
		//printk("DpcForIsr_Video3 data = [%d]%d \n",i,curr_buf_index);
		
		ret = SetQuene(pdx,i);
		if(ret != 0 ) 
		{
			//spin_unlock(&pdx->video_lock[i]);
			//mutex_unlock(&pdx->video_mutex[i]);
			return;
		}
		
		if(pdx->m_bVCapStarted[i] == TRUE)
		{
			
			if(pdx->m_bVCapIntDone[i] == TRUE && pdx->m_pVideoEvent[i])
			{
                pdx->m_bVCapIntDone[i] = FALSE;
				if(!pdx->m_bChangeVideoSize[i]) 
				{
					if((!pdx->m_bChangeVideoSize[i])&&(pdx->m_pVideoEvent[i])) 
					{
					//KeSetEvent(pdx->m_pVideoEvent[i], 0, FALSE);
					 //printk("SetEvenT[%d]\n",i);
					 //kill_fasync (&hw_async_video3, SIGIO, POLL_IN);
					 //pdx->wq_flag[i] = 1;
					 //wake_up_interruptible(&pdx->wq_video[i]); 
					  queue_work(pdx->wq,&pdx->video[i].videowork);
					}
				}
				else
				{
					pdx->m_bChangeVideoSize[i] = 0;
				}
			}
		}
	//spin_unlock(&pdx->video_lock[i]);	
	//mutex_unlock(&pdx->video_mutex[i]);
		
}	
#endif
//-----------------------------
/* Interrupt handler. Read/modify/write the command register to disable
 * the interrupt. */
//static irqreturn_t irqhandler(int irq, struct uio_info *info)
static irqreturn_t irqhandler(int irq, void  *info)
{
	struct hws_pcie_dev *pdx = (struct hws_pcie_dev *)(info);
	//struct pci_dev *pdev = pdx->pdev;
	
	u32 dma_status;
	u32 Int_Value =0;
	u32 IntState;
	u32 tmp;
	u32 cnt;
			
			dma_status =  READ_REGISTER_ULONG(pdx,(u32)(CVBS_IN_BASE));
			//printk("dma_status %x\n", dma_status);
			if(((dma_status&0x04)==0x04)&&(dma_status !=0xffffffff))
			{
				IntState= READ_REGISTER_ULONG(pdx,(u32)(CVBS_IN_BASE + 1 * PCIE_BARADDROFSIZE));
				if(IntState>0)
				{
					for(cnt =0; cnt <100; cnt ++)				
					{
					if(IntState == 0) break;
					if((IntState&0x01) == 0x01) // CH0  done
					{
						pdx->m_bVCapIntDone[0] = 1;
					
					Int_Value +=  0x01;
					if(pdx->m_nVideoBusy[0] ==0  )
					{
						tmp = (READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE + (32+0) * PCIE_BARADDROFSIZE)))&0x01;
						if(pdx->video_data[0] != tmp)
						{
							pdx->video_data[0]= tmp;
							pdx->m_nVideoBufferIndex[0]  = tmp;
							tasklet_schedule(&pdx->dpc_video_tasklet[0]);  // tasklet_hi_schedule
							//printk("Set OnInterrupt %x %d %d\n", 0,tmp,tmp2);
						}
					}
							
			 	 	}
					#if 0
			 		if((IntState&0x02) == 0x02) // CH1  done
			 		{
					//printk("OnInterrupt %x\n", 1);
					pdx->m_bVCapIntDone[1] = 1;
		
					Int_Value +=  0x02;
					if(pdx->m_nVideoBusy[1] ==0  )
					{
						 tmp = (READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE + (32+1) * PCIE_BARADDROFSIZE)))&0x01;
						if(pdx->video_data[1] != tmp)
						{
							pdx->m_nVideoBufferIndex[1] =  tmp;
							pdx->video_data[1]= pdx->m_nVideoBufferIndex[1];
							tasklet_schedule(&pdx->dpc_video_tasklet[1]);  
						}
					}
					
					}
				if((IntState&0x04) == 0x04) // CH2  done
				{
				//printk("OnInterrupt %x\n", 2);
				pdx->m_bVCapIntDone[2] = 1;
		
				Int_Value +=  0x04;
				if(pdx->m_nVideoBusy[2] ==0  )
				{
					 tmp = (READ_REGISTER_ULONG(pdx,(+ CVBS_IN_BASE + (32+2) * PCIE_BARADDROFSIZE)))&0x01;
					if(pdx->video_data[2] != tmp)
					{
						pdx->m_nVideoBufferIndex[2] = tmp;
						pdx->video_data[2]= pdx->m_nVideoBufferIndex[2];
						tasklet_schedule(&pdx->dpc_video_tasklet[2]);  
					}
				}
				}
				if((IntState &0x08) == 0x08) // CH1=3  done
				{
				//printk("OnInterrupt %x\n", 3);
				pdx->m_bVCapIntDone[3] = 1;
	
				Int_Value +=  0x08;
			
				if(pdx->m_nVideoBusy[3] ==0  )
				{
					 tmp = (READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE + (32+3) * PCIE_BARADDROFSIZE)))&0x01;
					if(pdx->video_data[3] != tmp)
					{
						pdx->m_nVideoBufferIndex[3] = tmp;
						pdx->video_data[3]= pdx->m_nVideoBufferIndex[3];
						//printk("OnInterrupt-%x [1] %d\n", 3,video_data[3]);
						tasklet_schedule(&pdx->dpc_video_tasklet[3]);  
					}
				}
				}
			#endif
			//-------
			//------------------------------
			
			if((IntState &0x100) == 0x100) // Audio ch0 done
			{
	
				Int_Value +=  0x100;
				//printk("OnInterrupt Audio  %x\n", 0);
				tmp = (READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE + (40+0) * PCIE_BARADDROFSIZE)))&0x01;
				pdx->m_nAudioBufferIndex[0] = tmp;
				pdx->audio_data[0]= pdx->m_nAudioBufferIndex[0];
				WRITE_ONCE(pdx->audio[0].last_irq_ns, ktime_get_ns());
				tasklet_schedule(&pdx->dpc_audio_tasklet[0]); 
			}
			#if 0
			if((IntState &0x200) == 0x200) // Audio ch1 done
			{
	
				Int_Value +=  0x200;
				if(pdx->m_nAudioBusy[1] ==0 )
				{
					tmp = (READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE + (40+1) * PCIE_BARADDROFSIZE)))&0x01;
					pdx->m_nAudioBufferIndex[1] = tmp;
					pdx->audio_data[1]= pdx->m_nAudioBufferIndex[1];
					tasklet_schedule(&pdx->dpc_audio_tasklet[1]); 
				}
			}
			if((IntState &0x400) == 0x400) // Audio ch2 done
			{
	
				Int_Value +=  0x400;
				
				if(pdx->m_nAudioBusy[2] ==0 )
				{
					//DbgPrint("OnInterrupt Audio ch-%x pdx->m_nAudioBusy[2] =%d\n", 2,pdx->m_nAudioBusy[2]);
					tmp = (READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE + (40+2) * PCIE_BARADDROFSIZE)))&0x01;
					pdx->m_nAudioBufferIndex[2] = tmp;
					pdx->audio_data[2]= pdx->m_nAudioBufferIndex[2];
					tasklet_schedule(&pdx->dpc_audio_tasklet[2]); 
				}
			}
			if((IntState &0x800) == 0x800) // Audio ch3 done
			{
	
				Int_Value +=  0x800;
				//DbgPrint("OnInterrupt Audio ch-%x pdx->m_nAudioBusy[3] =%d\n", 3,pdx->m_nAudioBusy[3]);
				if(pdx->m_nAudioBusy[3] ==0 )
				{
					tmp = (READ_REGISTER_ULONG(pdx,(CVBS_IN_BASE + (40+3) * PCIE_BARADDROFSIZE)))&0x01;
					pdx->m_nAudioBufferIndex[3] = tmp;
					pdx->audio_data[3]= pdx->m_nAudioBufferIndex[3];
					tasklet_schedule(&pdx->dpc_audio_tasklet[3]); 
				}
			}
			#endif 
			//--------------
			Int_Value = Int_Value&0x00ffffff;				
			WRITE_REGISTER_ULONG(pdx,(u32)(0x4000 + (PCIE_BARADDROFSIZE * 1)), Int_Value);
			IntState= READ_REGISTER_ULONG(pdx,(u32)(CVBS_IN_BASE + 1 * PCIE_BARADDROFSIZE));				
			if(IntState == 0) break;					
			}
					//printk("OnInterrupt IRQ_HANDLED  %X\n", pdev->device);
					return IRQ_HANDLED;
			}
			else
			{
					//printk("OnInterrupt[1] IRQ_NONE  %X\n", pdev->device);
					return IRQ_NONE;
			}
		}
		else
		{
				//printk("OnInterrupt[1] IRQ_NONE  %X\n", pdev->device);
				return IRQ_NONE;
		}

	//-------------------------------
	
}

static struct hws_pcie_dev *alloc_dev_instance(struct pci_dev *pdev)
{
	//int i;
	struct hws_pcie_dev *lro;

	if (!pdev) {
		pr_err("hws: alloc_dev_instance called with NULL pdev\n");
		return NULL;
	}

	/* allocate zeroed device book keeping structure */
	lro = kzalloc(sizeof(struct hws_pcie_dev), GFP_KERNEL);
	if (!lro) {
		printk("Could not kzalloc(hws_pcie_dev).\n");
		return NULL;
	}

	//lro->magic = MAGIC_DEVICE;
	//lro->config_bar_idx = -1;
	//lro->user_bar_idx = -1;
	//lro->bypass_bar_idx = -1;
	lro->irq_line = -1;

	/* create a device to driver reference */
	dev_set_drvdata(&pdev->dev, lro);
	/* create a driver to device reference */
	lro->pdev = pdev;
	//printk("probe() lro = 0x%p\n", lro);

	/* Set up data user IRQ data structures */
	//for (i = 0; i < MAX_USER_IRQ; i++) {
	//	lro->user_irq[i].lro = lro;
	//	spin_lock_init(&lro->user_irq[i].events_lock);
	//	init_waitqueue_head(&lro->user_irq[i].events_wq);
	//}

	return lro;
}


static void SetDMAAddress(struct hws_pcie_dev *pdx)
{
	//-------------------------------------

	u32 Addrmsk;
	u32 AddrLowmsk;
	//u32 AddrPageSize;
	//u32 Addr2PageSize;
	u32 PhyAddr_A_Low;
	u32 PhyAddr_A_High;
	
	//u32 PhyAddr_A_Low2;
	//u32 PhyAddr_A_High2;
	//u32 PCI_Addr2;
	
	u32 PCI_Addr;
	//u32 AVALON_Addr;
	u32 cnt;
	//u64 m_tmp64cnt = 0;
	//u32 RDAvalon = 0;
	//u32 m_AddreeSpace = 0;
	int i = 0;
	u32 m_ReadTmp __maybe_unused;
	u32 m_ReadTmp2 __maybe_unused;
	//u32 m_ReadTmp3;
	//u32 m_ReadTmp4;
	DWORD halfframeLength=0;
	//DWORD m_Valude;
	PhyAddr_A_High = 0;
	PhyAddr_A_Low=0;
	PCI_Addr =0;
    
	
	//------------------------------------------ // re write dma register 

	Addrmsk = PCI_E_BAR_ADD_MASK;
	AddrLowmsk = PCI_E_BAR_ADD_LOWMASK;

	//printk("[MV]1DispatchCreate :Addrmsk = %X  AddrPageSize =%X Addr2PageSize =%X \n", Addrmsk, AddrPageSize, Addr2PageSize);
	
	cnt = 0x208;  // Table address
	for (i = 0; i< pdx->m_nMaxChl; i++)
	{
		//printk("[MV] pdx->m_pbyVideoBuffer[%d]=%x\n", i, pdx->m_pbyVideoBuffer[i]);
		if (pdx->m_pbyVideoBuffer[i])
		{
				PhyAddr_A_Low = pdx->m_dwVideoBuffer[i];
				PhyAddr_A_High = pdx->m_dwVideoHighBuffer[i];
				
				PCI_Addr = (PhyAddr_A_Low&AddrLowmsk);
				PhyAddr_A_Low = (PhyAddr_A_Low&Addrmsk);
				
				
				//printk("[MV]1-pdx->m_dwVideoBuffer[%d]-%X\n",i,pdx->m_dwVideoBuffer[i]);
				//-------------------------------------------------------------------------------
				WRITE_REGISTER_ULONG(pdx,(PCI_ADDR_TABLE_BASE + cnt),PhyAddr_A_High);
				WRITE_REGISTER_ULONG(pdx,(PCI_ADDR_TABLE_BASE + cnt+PCIE_BARADDROFSIZE),PhyAddr_A_Low);  //Entry 0
				//----------------------------------------
				m_ReadTmp =  READ_REGISTER_ULONG(pdx,(PCI_ADDR_TABLE_BASE + cnt));
				m_ReadTmp2 = READ_REGISTER_ULONG(pdx,(PCI_ADDR_TABLE_BASE + cnt+PCIE_BARADDROFSIZE));
				//printk("[MV]1-PCI_Addr[%d] :PhyAddr_A_Low  %X=%X  PhyAddr_A_High %X=%X\n", i, PhyAddr_A_Low, m_ReadTmp2, PhyAddr_A_High, m_ReadTmp);
			   	

				//--------------------------
				WRITE_REGISTER_ULONG(pdx,( CBVS_IN_BUF_BASE + (i*PCIE_BARADDROFSIZE)), ((i+1)*PCIEBAR_AXI_BASE)+PCI_Addr); //Buffer 1 address
				halfframeLength = pdx->m_format[i].HLAF_SIZE/16;
				WRITE_REGISTER_ULONG(pdx,( CBVS_IN_BUF_BASE2 + (i*PCIE_BARADDROFSIZE)),halfframeLength); //Buffer 1 address

				
				m_ReadTmp =  READ_REGISTER_ULONG(pdx,(  CBVS_IN_BUF_BASE + (i*PCIE_BARADDROFSIZE)));
				m_ReadTmp2 = READ_REGISTER_ULONG(pdx,(  CBVS_IN_BUF_BASE2 + (i*PCIE_BARADDROFSIZE)));
				//printk("[MV]1-Avalone [X64]BUF[%d]:BUF1=%X  BUF2=%X\n", i,  m_ReadTmp,  m_ReadTmp2);
			
				//---------------------------
				
		}
		cnt +=8;	
		#if 1
		if(pdx->m_pbyAudioBuffer[i])
		{
				PhyAddr_A_Low = pdx->m_dwAudioBuffer[i];
				PhyAddr_A_High = pdx->m_dwAudioBufferHigh[i];
				PCI_Addr = (PhyAddr_A_Low&AddrLowmsk);
				PhyAddr_A_Low = (PhyAddr_A_Low&Addrmsk);
				//printk("[X1]Audio:PCI_Addr =%X\n",PCI_Addr);
				//printk("[X1]Audio:-------- - LOW=%X  HIGH =%X\n",pdx->m_dwAudioBuffer[i],pdx->m_dwAudioBufferHigh[i]);
				WRITE_REGISTER_ULONG(pdx,(CBVS_IN_BUF_BASE + ((8+i)*PCIE_BARADDROFSIZE)), ((i+1)*PCIEBAR_AXI_BASE+PCI_Addr)); //Buffer 1 address
				m_ReadTmp = READ_REGISTER_ULONG(pdx,(CBVS_IN_BUF_BASE + ((8+i)*PCIE_BARADDROFSIZE)));
		    	//printk("[X1]Audio:[%d] :--------BUF1: %X=%X\n",i,(PCIEBAR_AXI_BASE+PCI_Addr),m_ReadTmp);
		}
		#endif 
	}
	WRITE_REGISTER_ULONG(pdx,INT_EN_REG_BASE, 0x3ffff); //enable PCI Interruput		
	//WRITE_REGISTER_ULONG(PCIEBR_EN_REG_BASE, 0xFFFFFFFF);	
	
}

//-----------------------------------
static void ChangeVideoSize(struct hws_pcie_dev *pdx,int ch,int w,int h,int interlace)
{
	int j;
	int halfframeLength[4];
	unsigned long flags;
	if(ch != 0) return;
	if(SetVideoFormteSize(pdx,ch,w,h) != 1)
	{
		return;		
	}
	spin_lock_irqsave(&pdx->videoslock[ch], flags);
	for (j = 0; j<MAX_VIDEO_QUEUE; j++)
	{
		pdx->m_pVCAPStatus[ch][j].dwWidth = w ;
		pdx->m_pVCAPStatus[ch][j].dwHeight = h;
		pdx->m_pVCAPStatus[ch][j].dwinterlace = interlace;
				
	}
	spin_unlock_irqrestore(&pdx->videoslock[ch], flags);
	halfframeLength[0] = pdx->m_format[0].HLAF_SIZE/16;
	halfframeLength[1] = pdx->m_format[1].HLAF_SIZE/16;
	halfframeLength[2] = pdx->m_format[2].HLAF_SIZE/16;
	halfframeLength[3] = pdx->m_format[3].HLAF_SIZE/16;
	WRITE_REGISTER_ULONG(pdx,(DWORD)(CBVS_IN_BUF_BASE2 + (0*PCIE_BARADDROFSIZE)), halfframeLength[0]); //Buffer 1 address
	WRITE_REGISTER_ULONG(pdx,(DWORD)(CBVS_IN_BUF_BASE2 + (1*PCIE_BARADDROFSIZE)), halfframeLength[1]); //Buffer 1 address
	WRITE_REGISTER_ULONG(pdx,(DWORD)(CBVS_IN_BUF_BASE2 + (2*PCIE_BARADDROFSIZE)), halfframeLength[2]); //Buffer 1 address
	WRITE_REGISTER_ULONG(pdx,(DWORD)(CBVS_IN_BUF_BASE2 + (3*PCIE_BARADDROFSIZE)), halfframeLength[3]); //Buffer 1 address

	
}

static int Get_Video_Status(struct hws_pcie_dev *pdx,unsigned int  ch)
{
	int value; 
	int res_w=0;
	int res_h=0;
//	int frame_rate=0;
	int active_video=1;
	int interlace=0;
	int offset;
	int no_video;
	value =  READ_REGISTER_ULONG(pdx,(DWORD)(CVBS_IN_BASE + (5*PCIE_BARADDROFSIZE)));
	//printk("[MV]check NoVideo End: [%d] %X\n",ch,value);
	active_video = ((value&0xFF)>>ch)&0x01;
	interlace = value>>8;
	interlace = ((interlace&0xFF)>>ch)&0x01;
	//printk("[MV][%d] active_video %d\n",ch,active_video);
	if(active_video >0)
	{
			offset = 90 + ch*2;
			//DbgPrint("[MV][%d] active_video %d\n",ch,interlace);
			value =  READ_REGISTER_ULONG(pdx,(DWORD)(CVBS_IN_BASE + (offset*PCIE_BARADDROFSIZE)));
			res_w = value&0xFFFF;
			res_h = (value>>16)&0xFFFF;
			if(pdx->m_DeviceHW_Version==0)
			{
				if(res_w>3840) res_w = 3840;
				if(res_h>2160) res_h = 2160;
			}
			if(((res_w <=MAX_VIDEO_HW_W) &&(res_h<=MAX_VIDEO_HW_H)&&(interlace==0))||((res_w <=MAX_VIDEO_HW_W) &&(res_h*2<=MAX_VIDEO_HW_H)&&(interlace==1)))
			{
				if((res_w !=pdx->m_pVCAPStatus[ch][0].dwWidth )||(res_h!= pdx->m_pVCAPStatus[ch][0].dwHeight)||(pdx->m_pVCAPStatus[ch][0].dwinterlace!=interlace))
				{
					ChangeVideoSize(pdx,ch,res_w,res_h,interlace);
				
				}
			}
			
		no_video = 0;
		//printk("[MV-X1]-[ch-%d]W=%d H=%d interlace =%d %dx%d \n",ch,res_w,res_h,interlace,pdx->m_pVCAPStatus[ch][0].dwWidth, pdx->m_pVCAPStatus[ch][0].dwHeight);	
	}
	else
	{
		no_video = 1;
	}
	
	return no_video;
	
}

static void CheckVideFmt (struct hws_pcie_dev *pdx)
{
	//PAGED_CODE();
	int i;
	//DWORD value;
	//DWORD SetData;
//	int ret=0;
	//int nNeed_ReInit =0;
//	unsigned char mark=1;


		for(i =0; i<pdx->m_nCurreMaxVideoChl;i++)
		{
			#if 0
			value =  ReadDevReg((DWORD)(CVBS_IN_BASE + ((91+i*2)*PCIE_BARADDROFSIZE)));
			m_brightness[i] = value&0xFF;
			m_contrast[i] =  (value>>8)&0xFF;
			m_hue[i] = (value>>16)&0xFF;
			m_saturation[i] = (value>>24)&0xFF;
			//DbgPrint("[MV]value[%d]= %X\n",i,value);
			if((g_contrast[i] != m_contrast[i])||(g_brightness[i] != m_brightness[i])||(g_saturation[i] != m_saturation[i])||(g_hue[i] != m_hue[i]))
			{
				
				//DbgPrint("[MV]m_brightness[%d]= %d %d \n",i,m_brightness[i],g_brightness[i]);
				//DbgPrint("[MV]m_contrast[%d]= %d %d\n",i,m_contrast[i],g_contrast[i]);
				//DbgPrint("[MV]m_hue[%d]= %d %d \n",i,m_hue[i],g_hue[i]);
				//DbgPrint("[MV]m_saturation[%d]= %d %d \n",i,m_saturation[i],g_saturation[i] );
				SetData = g_saturation[i]<<24;
				SetData  |=g_hue[i]<<16;
				SetData  |=g_contrast[i]<<8;
				SetData  |=g_brightness[i];
				//DbgPrint("[MV]value[%d]= %X %X\n",i,value,SetData);
				WriteDevReg((DWORD)(CVBS_IN_BASE + ((91+i*2)*PCIE_BARADDROFSIZE)), SetData);
				
			}
			#endif 
			pdx->m_curr_No_Video[i] = Get_Video_Status(pdx,i);
			//---------------
			if((pdx->m_curr_No_Video[i] ==0x1)&&(pdx->m_bVCapStarted[i]==TRUE))
			{
				 //printk("[MV]check NoVideo End: [%d]\n",i);
				 queue_work(pdx->wq,&pdx->video[i].videowork);	
			}
			//-----------------
			
		}
		
}

static int MainKsThreadHandle(void *arg)
{
        int need_check=0;
		int i=0;
		struct hws_pcie_dev *pdx = (struct hws_pcie_dev *)(arg);
        while(1)
        {
              
			need_check=0;
			for(i=0; i<pdx->m_nMaxChl; i++)
			{
					if(pdx->m_bVCapStarted[i] ==1)
					{
						need_check = 1;
						break;
					}
			}
			if(need_check==1)
			{
				CheckVideFmt(pdx);
			}
            ssleep(1);
       		if(kthread_should_stop())
            {
                        break;
             }

        }
		//printk("MainKsThreadHandle Exit");
        return 0;
}
static void StartKSThread(struct hws_pcie_dev *pdx)
{
	    pdx->mMain_tsk = kthread_run(MainKsThreadHandle,(void*)pdx,"StartKSThread task"); 
	
}



//------------------------------


#ifndef arch_msi_check_device
static int arch_msi_check_device(struct pci_dev *dev, int nvec, int type)
{
	return 0;
}
#endif

/* type = PCI_CAP_ID_MSI or PCI_CAP_ID_MSIX */
static int msi_msix_capable(struct pci_dev *dev, int type)
{
	struct pci_bus *bus;
	int ret;
    //printk("msi_msix_capable in \n");
	if (!dev || dev->no_msi)
	{
		 printk("msi_msix_capable no_msi exit \n");
		return 0;
	}

	for (bus = dev->bus; bus; bus = bus->parent)
	{
		if (bus->bus_flags & PCI_BUS_FLAGS_NO_MSI)
		{
			printk("msi_msix_capable PCI_BUS_FLAGS_NO_MSI \n");
			return 0;
		}
	}
	ret = arch_msi_check_device(dev, 1, type);
	if (ret)
	{
		return 0;
	}
	ret = pci_find_capability(dev, type);
	if (!ret)
	{
		printk("msi_msix_capable pci_find_capability =%d\n",ret);
		return 0;
	}

	return 1;
}


static int probe_scan_for_msi(struct hws_pcie_dev *lro, struct pci_dev *pdev)
{
	//int i;
	int rc = 0;
	//int req_nvec = MAX_NUM_ENGINES + MAX_USER_IRQ;

	//BUG_ON(!lro);
	//BUG_ON(!pdev);
	//if (msi_msix_capable(pdev, PCI_CAP_ID_MSIX)) {
	//		printk("Enabling MSI-X\n");
	//		for (i = 0; i < req_nvec; i++)
	//			lro->entry[i].entry = i;
	//
	//		rc = pci_enable_msix(pdev, lro->entry, req_nvec);
	//		if (rc < 0)
	//			printk("Couldn't enable MSI-X mode: rc = %d\n", rc);
	
	//		lro->msix_enabled = 1;
	//		lro->msi_enabled = 0;
	//	} 
	//else  

	if (msi_msix_capable(pdev, PCI_CAP_ID_MSI)) {
		/* enable message signalled interrupts */
		//printk("pci_enable_msi()\n");
		rc = pci_enable_msi(pdev);
		if (rc < 0)
		{
			printk("Couldn't enable MSI mode: rc = %d\n", rc);
		}
		lro->msi_enabled = 1;
		lro->msix_enabled = 0;
	} else {
		//printk("MSI/MSI-X not detected - using legacy interrupts\n");
		lro->msi_enabled = 0;
		lro->msix_enabled = 0;
	}

	return rc;
}



static int irq_setup(struct hws_pcie_dev *lro, struct pci_dev *pdev)
{
	int rc = 0;
	u32 irq_flag;
	u8 val;
	//void *reg;
	//u32 w;

	//BUG_ON(!lro);

	//if (lro->msix_enabled) {
	//	rc = msix_irq_setup(lro);
	//} 
	//else 
	{
		if (!lro->msi_enabled){
			pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &val);
			//printk("Legacy Interrupt register value = %d\n", val);
		}
		//irq_flag = lro->msi_enabled ? 0 : IRQF_SHARED;
		irq_flag = lro->msi_enabled ? IRQF_SHARED:0;
		//irq_flag = IRQF_SHARED;
		
		rc = request_irq(pdev->irq, irqhandler, irq_flag, pci_name(pdev), lro); // IRQF_TRIGGER_HIGH 
		if (rc)
		{
			//printk("Couldn't use IRQ#%d, rc=%d\n", pdev->irq, rc);
		}
		else
		{
			lro->irq_line = (int)pdev->irq;
			//printk("Using IRQ#%d with  MSI_EN=%d \n", pdev->irq,lro->msi_enabled);
		}
	}

	return rc;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,5,0)
static void enable_pcie_relaxed_ordering(struct pci_dev *dev)
{
	pcie_capability_set_word(dev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_RELAX_EN);
}
#else
static void __devinit enable_pcie_relaxed_ordering(struct pci_dev *dev)
{
	u16 v;
	int pos;

	pos = pci_pcie_cap(dev);
	if (pos > 0) {
		pci_read_config_word(dev, pos + PCI_EXP_DEVCTL, &v);
		v |= PCI_EXP_DEVCTL_RELAX_EN;
		pci_write_config_word(dev, pos + PCI_EXP_DEVCTL, v);
	}
}
#endif
//--------------------------------------
static void InitVideoSys(struct hws_pcie_dev *pdx,int set)
{
	// init decoder 
	int i,j;
//	DWORD dwRest=0;
	DWORD m_Valude;
	if(pdx->m_bStartRun&&(set==0)) return;
	WRITE_REGISTER_ULONG(pdx,( CVBS_IN_BASE + (0 * PCIE_BARADDROFSIZE)), 0X00);
	SetDMAAddress(pdx);
	if(set ==0)
	{
		for(i=0; i<pdx->m_nMaxChl;i++)
		{
			for (j = 0; j<MAX_VIDEO_QUEUE; j++)
			{
				pdx->m_pVCAPStatus[i][j].byLock = MEM_UNLOCK;
				pdx->m_pVCAPStatus[i][j].byPath = 2;
				pdx->m_pVCAPStatus[i][j].byField = 0;
				pdx->m_pVCAPStatus[i][j].dwinterlace =0;
			}
			//pdx->m_nVideoIndex[i] =0;
			pdx->m_nAudioBufferIndex[i] =0; 
			EnableVideoCapture(pdx,i,0);
			EnableAudioCapture(pdx,i,0);
	
		}
	}

	WRITE_REGISTER_ULONG(pdx,INT_EN_REG_BASE, 0x3ffff);
	//start Run
	//---------------------------------------------
	WRITE_REGISTER_ULONG(pdx,CVBS_IN_BASE, 0x80000000);
	//DelayUs(500);	
    m_Valude= 0x00FFFFFF;
	m_Valude |= 0x80000000;		  
	WRITE_REGISTER_ULONG(pdx,CVBS_IN_BASE, m_Valude);
	WRITE_REGISTER_ULONG(pdx,(CVBS_IN_BASE + (0 * PCIE_BARADDROFSIZE)), 0X13);
	pdx->m_bStartRun = 1;
	//--------------------------------------------------
	
}

//-------------------------------------
static void SetHardWareInfo(struct hws_pcie_dev *pdx)
{
	switch (pdx->dwDeviceID)
	 {
		 default:
		 {
			   pdx->m_nCurreMaxVideoChl = 1;
			   pdx->m_nCurreMaxLineInChl =0;
			   pdx->m_MaxHWVideoBufferSize = MAX_MM_VIDEO_SIZE;
		   break;
		 }
	   }
	 //-----------------------
		if(pdx->m_Device_Version>121)
	   {
	   		pdx->m_DeviceHW_Version =1;
	   }
	   else
	   {
		   pdx->m_DeviceHW_Version =0;
	   }

}
static int ReadChipId(struct hws_pcie_dev *pdx)
{
	//  CCIR_PACKET      reg;
	//int Chip_id1 = 0;
	int ret=0;
	//int reg_vaule = 0;
	//int nResult;
	//------read Dvice Version
	ULONG m_dev_ver;
	ULONG m_tmpVersion;
	ULONG m_tmpHWKey;
	//ULONG m_OEM_code_data;
	m_dev_ver= READ_REGISTER_ULONG(pdx,CVBS_IN_BASE+(88*PCIE_BARADDROFSIZE));
  
		m_tmpVersion = m_dev_ver>>8;
		pdx->m_Device_Version =  (m_tmpVersion&0xFF);
		m_tmpVersion = m_dev_ver>>16;
		pdx->m_Device_SubVersion = (m_tmpVersion&0xFF);
		pdx->m_Device_SupportYV12 = ((m_dev_ver>>28)&0x0F);
		m_tmpHWKey =  m_dev_ver>>24;
		m_tmpHWKey = m_tmpHWKey&0x0F;
		pdx->m_Device_PortID = m_tmpHWKey&0x03;
		//n_VideoModle =	READ_REGISTER_ULONG(pdx,0x4000+(4*PCIE_BARADDROFSIZE));
		//n_VideoModle = (n_VideoModle>>8)&0xFF;
		//pdx->m_IsHDModel = 1;
	 	pdx->m_MaxHWVideoBufferSize = MAX_MM_VIDEO_SIZE;
	 	pdx->m_nMaxChl  = 4;
	 	pdx->m_bBufferAllocate = FALSE;
		pdx->mMain_tsk = NULL;
		pdx->m_dwAudioPTKSize = MAX_DMA_AUDIO_PK_SIZE; //128*16*4;
		pdx->m_bStartRun = 0;
		pdx->m_PciDeviceLost =0;
		
		WRITE_REGISTER_ULONG(pdx,CVBS_IN_BASE,0x0);
		//ssleep(100);
		WRITE_REGISTER_ULONG(pdx,CVBS_IN_BASE,0x10);
   		//ssleep(500);	
		//-------
		SetHardWareInfo(pdx);
		printk("************[HW]-[VIDV]=[%d]-[%d]-[%d] ************\n",  pdx->m_Device_Version, pdx->m_Device_SubVersion,pdx->m_Device_PortID );	
	return ret;

}

static int hws_probe(struct pci_dev *pdev, const struct pci_device_id *pci_id)
{
	struct hws_pcie_dev *gdev=NULL;
	int err = 0, ret = -ENODEV;
	//u8 val=0;
	//u32 m_dev_ver=0;
	//u32 m_dev_vid_ver=0;
	//u32 m_dev_supportYV12=0;
	//u32 m_Device_Version=0;
	//ULONG m_tmpHWKey;
	//ULONG m_tmpVersion;
	//u32 m_tmpVersion=0;
	//u32 m_Device_SubVersion=0;
	//u32 m_Device_SupportYV12=0;
	//ULONG n_VideoModle =0;
	//u64 *mem64_ptr;
	int j, i;
	//---------------------------
	//printk("hws_probe  probe\n");
	//------------------------
	gdev = alloc_dev_instance(pdev);
	//sys_dvrs_hw_pdx = gdev;
	gdev->pdev = pdev;
	
	gdev->dwDeviceID = gdev->pdev->device;
	gdev->dwVendorID = gdev->pdev->vendor;
	printk("MV360: Device =%X VID =%X\n",gdev->dwDeviceID,gdev->dwVendorID); 
	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "%s: pci_enable_device failed: %d\n",
				__func__, err);
		goto  err_alloc;
	}
	//printk("hws_probe  probe 2\n"); 
	#if 0
	if (pci_request_regions(pdev, "longtimetech"))
		goto err_disable;
	if (pci_set_dma_mask(pdev, DMA_BIT_MASK(32))) {
		printk(KERN_EMERG "fail pci_set_dma_mask\n");
       	goto err_release;
    }
	#endif 
	
	//printk("hws_probe  probe 3\n"); 
	enable_pcie_relaxed_ordering(pdev);
	//printk("hws_probe  probe 4\n"); 
	pci_set_master(pdev);
	//------------------------------------	
	ret = probe_scan_for_msi(gdev, pdev);	
	if (ret < 0)		
	  goto disable_msi;	
	//------------------------
	//printk("hws_probe  probe 5\n"); 
	/* known root complex's max read request sizes */
#ifdef CONFIG_ARCH_TI816X
	//dbg_init("TI816X RC detected: limit MaxReadReq size to 128 bytes.\n");
	pcie_set_readrq(pdev, 128);
#endif
 #if 0
	gdev->info.mem[0].addr = pci_resource_start(pdev, 0);
	if (!gdev->info.mem[0].addr)
		goto err_release;
  #endif 
  //printk("hws_probe  ioremap_nocache\n"); 
  #if 0
	gdev->info.mem[0].internal_addr = ioremap_nocache(pci_resource_start(pdev, 0), 
		pci_resource_len(pdev, 0));
  #else
        //gdev->info.mem[0].internal_addr = ioremap_cache(pci_resource_start(pdev, 0), 
  		//gdev->info.mem[0].internal_addr = ioremap_nocache(pci_resource_start(pdev, 0), 
		//pci_resource_len(pdev, 0));

		gdev->info.mem[0].internal_addr = ioremap(pci_resource_start(pdev, 0), 
		pci_resource_len(pdev, 0));
  
  #endif
  	gdev->wq=NULL;
	gdev->auwq=NULL;
	gdev->map_bar0_addr = (u32 *)gdev->info.mem[0].internal_addr;

	if (!gdev->info.mem[0].internal_addr)
		goto err_release;

	gdev->info.mem[0].size = pci_resource_len(pdev, 0);
	gdev->info.mem[0].memtype = UIO_MEM_PHYS;

  
	
	//printk(" pdev->irq = %d \n",pdev->irq); 
	ret = irq_setup(gdev, pdev);
	if (ret)
		goto err_register;

	//printk("pci_set_drvdata \n"); 
	pci_set_drvdata(pdev, gdev);
	//enable irq
	//enable_irq(gdev->info.irq);
	//------
	ReadChipId(gdev);
		//---------------
		for (i = 0; i<MAX_VID_CHANNELS; i++)
		{
			//gdev->m_nVideoIndex[i] =0;
			gdev->m_nRDVideoIndex[i] =0;
			gdev->m_bVCapIntDone[i] =0;
			gdev->m_nVideoBusy[i] = 0;
			gdev->m_bChangeVideoSize[i] =0;
			gdev->m_nVideoBufferIndex[i] = 0;
			gdev->m_nVideoHalfDone[i] =0;
			gdev->m_pVideoEvent[i] = 0;
			SetVideoFormteSize(gdev,i,1920,1080);
			gdev->m_bVCapStarted[i] = 0;
			gdev->m_bVideoStop[i]=0;
			gdev->video_data[i] =0;
			//----------------------

			gdev->m_pbyVideoBuffer[i] = NULL;
			gdev->m_VideoInfo[i].dwisRuning= 0;
			gdev->m_VideoInfo[i].m_nVideoIndex= 0;
			gdev->m_VideoInfo[i].m_pVideoScalerBuf = NULL;
			for (j = 0; j<MAX_VIDEO_QUEUE; j++)
			{
				gdev->m_pVCAPStatus[i][j].byLock = MEM_UNLOCK;
				gdev->m_pVCAPStatus[i][j].byField = 0;
				gdev->m_pVCAPStatus[i][j].byPath = 2;
				gdev->m_pVCAPStatus[i][j].dwWidth = 1920 ;
				gdev->m_pVCAPStatus[i][j].dwHeight = 1080;
				gdev->m_pVCAPStatus[i][j].dwinterlace =0;
				//gdev->m_pVideoData[i][j] = NULL;
				//------------------
				gdev->m_VideoInfo[i].m_pVideoBufData[j] = NULL;
				gdev->m_VideoInfo[i].m_pVideoBufData1[j] = NULL;
				gdev->m_VideoInfo[i].m_pVideoBufData2[j] = NULL;
				gdev->m_VideoInfo[i].m_pVideoBufData3[j] = NULL;
				gdev->m_VideoInfo[i].pStatusInfo[j].byLock= MEM_UNLOCK;
				//----------------
			}
			//--------audio
			gdev->m_pAudioEvent[i] = 0;
			gdev->m_bACapStarted[i]=0;
			gdev->m_bAudioRun[i] = 0;
			gdev->m_bAudioStop[i] = 0;
			gdev->m_nAudioBusy[i] = 0;
			gdev->m_nRDAudioIndex[i] =0;
			//sema_init(&gdev->sem_video[i],1);  
			//spin_lock_init(&gdev->video_lock[i]); 
			spin_lock_init(&gdev->videoslock[i]);
			spin_lock_init(&gdev->audiolock[i]);
			//mutex_init(&gdev->video_mutex[i]); 
			//init_waitqueue_head(&gdev->wq_video[i]);  
			//gdev->wq_flag[i]=0;
			gdev->m_AudioInfo[i].dwisRuning =0;
			gdev->m_AudioInfo[i].m_nAudioIndex =0;
			gdev->audio[i].resampled_buf =NULL;
			for(j=0; j<MAX_AUDIO_QUEUE;j++)
			{
				gdev->m_AudioInfo[i].m_pAudioBufData[j] =NULL;
				gdev->m_AudioInfo[i].pStatusInfo[j].byLock = MEM_UNLOCK;
				gdev->m_AudioInfo[i].m_pAudioBufData[j] = NULL;
			}
			//gdev->video[i].v4l2_dev = NULL;
		}
		//---------------------
	 	 tasklet_init(&gdev->dpc_video_tasklet[0],DpcForIsr_Video0,(unsigned long)gdev);
		 //tasklet_init(&gdev->dpc_video_tasklet[1],DpcForIsr_Video1,(unsigned long)gdev);
		 //tasklet_init(&gdev->dpc_video_tasklet[2],DpcForIsr_Video2,(unsigned long)gdev);
		 //tasklet_init(&gdev->dpc_video_tasklet[3],DpcForIsr_Video3,(unsigned long)gdev);

		 tasklet_init(&gdev->dpc_audio_tasklet[0],DpcForIsr_Audio0,(unsigned long)gdev);
		 //tasklet_init(&gdev->dpc_audio_tasklet[1],DpcForIsr_Audio1,(unsigned long)gdev);
		 //tasklet_init(&gdev->dpc_audio_tasklet[2],DpcForIsr_Audio2,(unsigned long)gdev);
		 //tasklet_init(&gdev->dpc_audio_tasklet[3],DpcForIsr_Audio3,(unsigned long)gdev);
		 
		//----------------------
	 	ret = DmaMemAllocPool(gdev);
		 if(ret !=0)
	  	{
			goto err_mem_alloc;
	   }
	   //SetDMAAddress(gdev);
	   InitVideoSys(gdev,0);
	   StartKSThread(gdev);
	 // just test
	 //StartVideoCapture(gdev,0);
	//-------------------
	//printk("hws_probe probe exit \n"); 
	//--------------------------------------
	//--------------------
	hws_adapters_init(gdev);
	gdev->wq =   create_singlethread_workqueue("hwsuhdx1");
	gdev->auwq = create_singlethread_workqueue("hwsuhdx1-audio");
	//----------------
	hws_diag_reset();
	hws_diag_init_debugfs();
	if( hws_video_register(gdev) )
		goto err_mem_alloc;
#if 1
		if(hws_audio_register(gdev))
		goto err_mem_alloc;
#endif	
	return 0;
err_mem_alloc:
hws_diag_remove_debugfs();
	
		 gdev->m_bBufferAllocate = TRUE;
		 DmaMemFreePool(gdev);
		 gdev->m_bBufferAllocate = FALSE;
err_register:
		iounmap(gdev->info.mem[0].internal_addr);
		irq_teardown(gdev);
		kfree(gdev);
disable_msi:	
		if (gdev->msix_enabled) 
		{		
		pci_disable_msix(pdev); 	
		gdev->msix_enabled = 0; 
		}	
		else if (gdev->msi_enabled)
		{
			pci_disable_msi(pdev);		
			gdev->msi_enabled = 0;	
		}
err_release:
		pci_release_regions(pdev);
		pci_disable_device(pdev);
		return err;
err_alloc:
			kfree(gdev);
			
	return	-1;


}

MODULE_DEVICE_TABLE(pci, hws_pci_table);

static struct pci_driver hws_pci_driver = {
	.name        = KBUILD_MODNAME,
	.id_table    = hws_pci_table,
	.probe       = hws_probe,
	.remove      = hws_remove,
};

static __init int pcie_hws_init(void)
{

	return pci_register_driver(&hws_pci_driver);
}

static __exit void pcie_hws_exit(void)
{
	pci_unregister_driver(&hws_pci_driver);
}

module_init(pcie_hws_init);
module_exit(pcie_hws_exit);

MODULE_DESCRIPTION("HWS driver");
MODULE_AUTHOR("Alex Liu <alex.liu@longtimetech.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
