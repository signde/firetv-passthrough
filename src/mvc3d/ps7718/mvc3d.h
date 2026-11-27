/* SPDX-License-Identifier: GPL-2.0-or-later */
/* clang-format off */
/*
 * mvc3d: hardware 3D MVC (Blu-ray 3D) playback with HDMI frame packing on the Fire TV Cube 3rd gen.
 *
 *   mvc3d_main.c  /dev/mvc3dhw, the vfm receiver/provider between amvdec_h264mvc and amvideo,
 *                 picture timing, module parameters, debugfs (hw_stats; vsynclog with debug=1)
 *   mvc3d_fp.c    HDMI frame packing on hdmitx21: mode set/timing probes, 3D VSIF and its keeper,
 *                 video layer fixes
 *   mvc3d_pool.c  the MVC decoder's work space and reference buffers moved below 2 GiB (its 31 bit
 *                 DMA)
 *
 * PS7718 dependencies (Fire OS 7.7.1.8, kernel 5.4.125-ab35, Fire TV Cube 3rd gen "gazelle",
 * Amlogic T7). Everything below is matched against that build; a missing symbol or a mismatching
 * layout/instruction turns the feature off at load (logged), nothing is written blind.
 *
 * symbol                          how        used for                              if missing
 * ------------------------------  ---------  ------------------------------------  -------------
 * main
 * set_freerun_mode                kallsyms   amvideo freerun while a session       load fails
 * _video_set_disable              kallsyms   VD1 back on after a seek              warning
 * aml_read_vcbus                  kallsyms   debug=1 scan-out checks, colours      no diagnostics
 * get_cur_enc_line                kallsyms   debug=1 vsync log                     no diagnostics
 * pool
 * amvdec_h264mvc:init_canvas      kprobe     next alloc = reference buffer         no hardware MVC
 * decoder_bmmu_box_alloc_buf_phy  kretprobe  buffers into the < 2 GiB pool         no hardware MVC
 *   (x1 idx, x2 size, x3 driver name, x4 &phys)
 * POOL_TOP 0x80000000,            physical   low DMA pool window (RAM layout)      pool refused
 *   POOL_BOTTOM 0x31000000
 * fp
 * get_hdmitx21_device,            kallsyms   hdmitx21 device, vinfo                no frame packing
 *   get_current_vinfo
 * hdmitx21_gettiming_from_vic     kallsyms,  CEA timings / FP timing on return     no frame packing
 *                                 kretprobe
 * hdmitx21_get_fmtpara            kretprobe  para/vinfo patched to the FP timing   no frame packing
 *   (callers hdmitx_set_current_vmode, set_disp_mode_auto, by %ps name)
 * hdmi_vend_infoframe_rawset      kallsyms   our HDMI 1.4b 3D VSIF                 no frame packing
 * set_video_window_ext            kallsyms   full-frame video window               window not set
 * hdmitx21_set_display            kretprobe  VSIF again after the mode set         probe off
 * set_tv_encp_new (x2 = VIC)      kretprobe  ENCP frame packed timing              probe off
 * hd21_write_reg, hd21_read_reg   kallsyms   ENCP regs 0x1b8d-0x1c3f (<< 2)        ENCP probe off
 * _tpi_infoframe_wrrd.llvm.6229976626646311496
 *                                 kretprobe  VSIF re-assert after foreign writes   probe off
 * hdmitx_infoframe_send, _rawget  kallsyms   VSIF keeper read-back/resend          keeper idle
 * set_hdcp2_topo                  kprobe     VSIF resend after HDCP re-auth        probe off
 * is_amdv_on, is_amdv_enable      kallsyms   debug=1 colour log                    -1 logged
 * meson_plane_atomic_check        kprobe     DRM plane check while FP              probe off
 * _set_video_window               kprobe     video window while FP                 probe off
 *
 * instruction fixes (fp; the 6 words from off - 0xc are checked first, else that fix is off):
 *   pre_process_for_3d+0x6c         5282000d 3215016b 5280002a b9000e6d 5280002d 14000003  vftype
 *   vpp_set_filters+0x7a8           17ffff06 378801d8 531f790a 5280004c b900be6c 2a0b03ec  vsf
 *   vd1_set_dcu+0x1048              52820229 6a09033f 54000440 12000509 35000409 f0008588  psel
 *   config_vd_position+0x420        b9464e8a 529ec00f 72a0064f 6a0f001f 54000600 b9005a6a  vdpos
 *   vpp_set_filters_internal+0x604  292fbba9 b9406fe9 b81b83a8 6b0a013f 54000089 b940aec8  vskip
 *
 * struct offsets (7.7.1.3 headers, checked by value at use):
 *   struct hdmitx_dev  fmt_attr +10192, para +184, flag_3dfp +10536 bit 0
 *   struct vframe_s    type +12, width +308, height +312, trans_fmt +360, left_eye +376,
 *                      right_eye +392
 * VPU registers (debug=1 only): VD1 0x4820/0x4825, VD2 0x48a0/0x48a5, canvas 0x1a51/0x1a71,
 *   the colour dump list
 *   other modules' parameters (set in-kernel through their kernel_param ops):
 *     amvdec_h264mvc  view_mode, drop_thread_hold
 *     aml_media       framepacking_support/_width/_height/_blank, dolby_vision_policy/_mode
 *   sysfs attributes, written/read through their own sysfs_ops (mvc3d_sys.c): class/display/mode,
 *     class/amhdmitx/amhdmitx0/attr, class/video/{video_global_output,disable_video,threedim_mode
 *     ("process type 0x%x,trans fmt %u.")}, class/graphics/fb0-2/{osd_display_debug,blank};
 *     /sys/class/vfm/map (default chain)
 */
/* clang-format on */
#ifndef MVC3D_H
#define MVC3D_H

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/hardirq.h>
#include <linux/workqueue.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/dma-mapping.h>
#include <drm/drm_plane.h>

#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#include <linux/amlogic/media/vfm/vfm_ext.h>
#include <linux/amlogic/media/registers/register_map.h>

#include "mvc3d_uapi.h"

#define HW_NAME "mvc3dhw"
#define HW_PROVIDER "decoder.h264mvc"
#define HW_MAP_ID "mvc3dhw"
#define HW_MAP_CHAIN HW_PROVIDER " " HW_NAME " amvideo"
#define NSLOT MVC3D_HW_MAX_BUFS

#include <linux/kallsyms.h>
#include <linux/kernfs.h>
#include <linux/sysfs.h>
#include <linux/amlogic/media/vout/vinfo.h>
#include <linux/amlogic/media/vout/hdmi_tx21/hdmi_tx_module.h>

#define MVC3D_MODULE_VERSION "2026.11.26.23-gazelle"

#ifndef MODE_3D_ENABLE
#define MODE_3D_ENABLE 0x00000001
#define MODE_3D_FA 0x00000020
#endif

/* physical top of the low DMA pool: below it bit 31 of an address is 0 */
#define POOL_TOP 0x80000000UL

enum slot_state { S_FREE, S_READY, S_CLIENT, S_QUEUED, S_SHOWN, S_RETIRED, S_DONE };

struct slot {
	struct vframe_s *vf;
	enum slot_state state;
	u32 seq, gen;
	u64 decoded_ns, due_ns;
	u32 retire_seq; /* S_RETIRED: the vsync it came back from the display at */
	u64 addr_l, addr_r; /* the two views' luma (diagnostics) */
};

struct sc2_dev {
	struct miscdevice misc;
	struct mutex mlock; /* ioctls start/stop, map */
	spinlock_t lock; /* slots, queues, prov_reg (also from the vsync interrupt) */
	wait_queue_head_t wq_client, wq_thread;
	bool kick;
	struct task_struct *thread;
	struct vframe_receiver_s recv;
	struct vframe_provider_s prov;
	struct dentry *dbg;

	struct file *owner;
	bool started, prov_reg, routed, fp;
	bool display; /* this session set up the display (own_display): put back at its stop */
	u32 seq, gen;
	struct slot slots[NSLOT];
	int ready[NSLOT], ready_head, ready_cnt; /* got from the decoder, for MVC3D_HW_WAIT */
	int dispq[NSLOT], disp_head, disp_cnt; /* presented, by due time */
	char default_chain[160];

	/* vsync period, measured from amvideo's peeks in its vsync interrupt */
	u64 vs_last_ns, vs_period_ns, vs_odd_ns;
	u32 vs_odd;
	u32 vs_seq; /* vsyncs seen */
	u32 put_next; /* the seq of the next picture the decoder frees */
	u64 retired_late;

	u64 pics_in, shown, dropped_late, dropped_client, released, put_back, stale_put;
	u64 late_us_sum, late_cnt;
	s64 late_max_us;
};

/* mvc3d_main.c */
extern struct sc2_dev *mvc3d_dev;
extern int mvc3d_debug;
extern int (*mvc3d_read_vcbus)(unsigned int reg);
int mvc3d_kparam(const char *mod, const char *name, const char *set, char *get, size_t gsz);
bool mvc3d_kparam_path(const char *path, char *mod, size_t msz, const char **name);
int mvc3d_sysfs_write(const char *path, const char *val);
void mvc3d_in_worker(void (*fn)(void *), void *arg);

/* mvc3d_fp.c */
extern bool mvc3d_fp_ok, mvc3d_fp_armed, mvc3d_fp_on;
void mvc3d_fp_init(void);
int mvc3d_fp_hooks(bool on);
void mvc3d_fp_exit(void);
void mvc3d_fp_arm(bool on);
int mvc3d_fp_3d_type(u32 type);
void mvc3d_fp_conv_vf(struct vframe_s *vf);
void mvc3d_fp_session_start(void);
void mvc3d_fp_summary(void);
void mvc3d_fp_stats(struct seq_file *m);

/* mvc3d_sys.c */
bool mvc3d_own_display(void);
int mvc3d_sys_read(const char *path, char *buf, size_t size);
int mvc3d_sys_write(const char *path, const char *val);
int mvc3d_display_take(void);
void mvc3d_display_give_back(void);

/* mvc3d_ui.c */
void mvc3d_ui3d_hooks(bool on, int base_h, int out_h);
void mvc3d_ui3d_want(bool want);
bool mvc3d_ui3d_ready(void);
bool mvc3d_ui3d_active(void);
u64 mvc3d_ui3d_stretched(void);

/* mvc3d_pool.c */
void mvc3d_pool_init(void);
void mvc3d_pool_exit(void);
bool mvc3d_pool_probes_on(void);
int mvc3d_pool_hooks(bool on);
int mvc3d_pool_get(struct device *dev);
void mvc3d_pool_put(void);
void mvc3d_pool_reset(void);
void mvc3d_pool_decoder_gone(void);
void mvc3d_pool_release_if_idle(struct sc2_dev *h);
void mvc3d_pool_stats(struct seq_file *m);

#endif /* MVC3D_H */
