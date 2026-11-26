// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3d: hardware 3D MVC (Blu-ray 3D) playback for the Fire TV Cube 3rd gen (gazelle, Amlogic T7,
 * Fire OS 7.7.1.8 PS7718, kernel 5.4.125-ab35). Ported from djnice's S905X4 (SC2) module
 * (vlc-fel-android kernel/mvc3d/sc2).
 *
 * The legacy MVC decoder (amvdec_h264mvc, provider "decoder.h264mvc") gives its pictures as canvas
 * indices here, which the legacy video layer (amvideo) shows as they are: view_mode 0 = the base
 * view (2D), view_mode 3 = both views (VIDTYPE_MVC; with framepacking_support the VPP itself puts
 * the right view under the left one, 1920x2205). Nothing is composed: while a client plays, the
 * vfm path is "decoder.h264mvc mvc3dhw amvideo" and this module holds the decoder's pictures until
 * the player names their display time (MVC3D_HW_PRESENT); the provider side gives amvideo (freerun:
 * it takes what it peeks) a picture only from the vsync near that time, and gives pictures back to
 * the decoder in the order it handed them out, put_delay_vsync vsyncs after the display let go.
 *
 * Gazelle specifics:
 *  - set_freerun_mode, get_hdmitx21_device, get_current_vinfo, ... are resolved with
 *    kallsyms_lookup_name (their CRCs are not in any Fire OS module's __versions);
 *  - the decoder's 31 bit DMA: its work space and reference buffers go to a pool below 2 GiB
 *    (mvc3d_pool.c) where RAM reaches 2 GiB; any buffer at or above 2 GiB is refused;
 *  - HDMI frame packing (mvc3d_fp.c) only with fp_enable=1: 1080p24/25/30 become 1920x2205 frame
 *    packed (2250 lines, 148.5 MHz) and 720p60/50 1280x1470, with the HDMI 1.4b 3D VSIF, while a
 *    client (or fp_test=1) arms it. Module parameters of other modules are set in-kernel (SELinux
 *    denies a kernel worker's sysfs writes). Without it the module plays the base view (2D).
 *
 * Every PS7718 symbol, offset and instruction this depends on is listed in mvc3d.h.
 * Diagnostics (buffer/scan-out checks, vsync log, colour state, session summary) only with debug=1.
 */
#include "mvc3d.h"

#define MVC_PARAMS "/sys/module/amvdec_h264mvc/parameters/"
#define FP_PARAM "/sys/module/aml_media/parameters/framepacking_support"

static void (*p_set_freerun_mode)(int mode);
/*
 * a seek re-opens the decoder: its unreg makes amvideo free its keep buffer, and
 * try_free_keep_video(1) leaves disable_video at VIDEO_DISABLE_NORMAL (1) - VD1 stays off for every
 * later picture (layer_swap_frame enables only if disable_video != NORMAL; a player normally writes
 * 0 again). A decoder registered while our session runs puts it back to VIDEO_DISABLE_NONE.
 */
static int (*p_video_set_disable)(u32 val);
static u32 reenables;
#define set_freerun_mode(m)                                                                        \
	do {                                                                                       \
		if (p_set_freerun_mode)                                                            \
			p_set_freerun_mode(m);                                                     \
	} while (0)
static int fp_enable;
module_param(fp_enable, int, 0444);
MODULE_PARM_DESC(
	fp_enable,
	"HDMI frame packing (hdmitx21 1080p24/25/30 frame packed; probes only, nothing changes until armed)");
static bool need_pool;
static unsigned long ram_top;

/* diagnostics (kernel log, debugfs vsynclog), off by default */
int mvc3d_debug;
module_param_named(debug, mvc3d_debug, int, 0644);
MODULE_PARM_DESC(
	debug,
	"diagnostics: 1 = log buffer/scan-out checks, colour state and per-session summaries");

/*
 * vh264_mvc's vf_put() does not look at the picture it is given: it advances its
 * putting pointer, and its recycle timer frees the OLDEST picture it handed out. A picture put back
 * out of order (a late or client drop, newer than the one on the display) so frees the buffer on
 * the display - the decoder, short of buffers while we hold up to NSLOT, writes the next picture
 * into it as the VPP scans it: a torn frame, the more visible the more the pictures differ (scene
 * cuts, where the big I pictures make drops). Pictures now go back in the order we got them (S_DONE
 * waits for the older ones).
 */
static u64 put_held, put_forced;

/*
 * debug=1, buffer reuse: a new picture from the decoder whose left (base) or right luma buffer is
 * one we still hold (the decoder wrote into it before we gave it back), and how long after we gave
 * a buffer back the decoder outputs it again, per view.
 */
#define RH_N 48
struct rh_rec {
	u64 addr, ns;
	u8 view;
};
static struct rh_rec rh_ring[RH_N];
static u32 rh_head;
static u64 reuse_held[2], reuse_dt[2][4], reuse_min_us[2] = { ~0ULL, ~0ULL };
static u64 isr_late;
static u32 reuse_logged;

static void rh_note_locked(u64 a, int view, u64 now)
{
	if (!a || !READ_ONCE(mvc3d_debug))
		return;
	rh_ring[rh_head % RH_N].addr = a;
	rh_ring[rh_head % RH_N].ns = now;
	rh_ring[rh_head % RH_N].view = view;
	rh_head++;
}

/*
 * A picture amvideo gives back may still be scanned out until the next vsync's RDMA has switched
 * VD1/VD2 to the next one, and the decoder may write its next picture into a buffer as soon as it
 * has it back. Given back 2 vsyncs after amvideo's put, the left eye's top still tore at scene cuts
 * on a heavy stream (Billy Lynn; CoreELEC, whose amvideo returns pictures at once, tears the same
 * on this box); 3 vsyncs (put_delay_vsync = 1, the default) does not.
 */
static int put_delay_vsync = 1;
module_param(put_delay_vsync, int, 0644);
MODULE_PARM_DESC(
	put_delay_vsync,
	"pictures back from the display go to the decoder 2 + this vsyncs later (0-4; 1: no tearing)");

struct sc2_dev *mvc3d_dev;
static u64 sc2_dma_mask = DMA_BIT_MASK(32);

/* ------------------------------------------------------------------ */
/* vfm route, decoder and video parameters                            */
/* ------------------------------------------------------------------ */

struct kw {
	struct work_struct work;
	const char *path, *val;
	int ret;
};

static void kw_fn(struct work_struct *work)
{
	struct kw *k = container_of(work, struct kw, work);
	struct file *f = filp_open(k->path, O_WRONLY, 0);
	loff_t pos = 0;
	ssize_t n;

	if (IS_ERR(f)) {
		k->ret = PTR_ERR(f);
		return;
	}
	n = kernel_write(f, k->val, strlen(k->val), &pos);
	filp_close(f, NULL);
	k->ret = n < 0 ? n : 0;
}

/*
 * Module parameters in-kernel: SELinux on Fire OS denies a kernel worker's writes to
 * sysfs (u:r:kernel:s0), so a /sys/module/<mod>/parameters/<name> is set through its kernel_param
 * ops, found by its sysfs node: /sys/module's kset is our own module kobject's kset, the node's
 * priv the attribute of a struct param_attribute (kernel/params.c, 5.4).
 */
struct param_attribute_ {
	struct module_attribute mattr;
	const struct kernel_param *param;
};

int mvc3d_kparam(const char *mod, const char *name, const char *set, char *get, size_t gsz)
{
	struct kset *ks = THIS_MODULE->mkobj.kobj.kset;
	struct kobject *ko;
	struct module_kobject *mk;
	struct kernfs_node *pd, *kn;
	const struct kernel_param *kp;
	size_t nl = strlen(name), kl;
	int r = -ENOENT;

	if (get && gsz)
		get[0] = 0;
	if (!ks)
		return -ENODEV;
	ko = kset_find_obj(ks, mod);
	if (!ko)
		return -ENOENT;
	mk = container_of(ko, struct module_kobject, kobj);
	pd = ko->sd ? kernfs_find_and_get(ko->sd, "parameters") : NULL;
	kn = pd ? kernfs_find_and_get(pd, name) : NULL;
	if (kn && kernfs_type(kn) == KERNFS_FILE && kn->priv) {
		kp = container_of((struct attribute *)kn->priv, struct param_attribute_, mattr.attr)
			     ->param;
		kl = kp && kp->name ? strlen(kp->name) : 0;
		/* the name check: "name" (a module) or "mod.name" (built in) */
		if (!kp || !kp->ops || kl < nl || strcmp(kp->name + kl - nl, name) ||
		    (kl > nl && kp->name[kl - nl - 1] != '.')) {
			r = -EINVAL;
		} else if (set) {
			kernel_param_lock(mk->mod);
			r = kp->ops->set ? kp->ops->set(set, kp) : -EPERM;
			kernel_param_unlock(mk->mod);
		} else {
			char *b = (char *)get_zeroed_page(GFP_KERNEL);

			r = -ENOMEM;
			if (b) {
				kernel_param_lock(mk->mod);
				r = kp->ops->get ? kp->ops->get(b, kp) : -EPERM;
				kernel_param_unlock(mk->mod);
				if (r >= 0 && get && gsz) {
					strscpy(get, b, gsz);
					get[strcspn(get, "\n")] = 0;
					r = 0;
				}
				free_page((unsigned long)b);
			}
		}
	}
	kernfs_put(kn);
	kernfs_put(pd);
	kobject_put(ko);
	return r;
}

/* "/sys/module/<mod>/parameters/<name>": mod and name, else false */
bool mvc3d_kparam_path(const char *path, char *mod, size_t msz, const char **name)
{
	const char *m = path + 12, *e;

	if (strncmp(path, "/sys/module/", 12))
		return false;
	e = strchr(m, '/');
	if (!e || e - m >= msz || strncmp(e, "/parameters/", 12) || !e[12])
		return false;
	memcpy(mod, m, e - m);
	mod[e - m] = 0;
	*name = e + 12;
	return true;
}

/*
 * The parameter files are root's (0664): written by a kernel worker, not with the credentials of
 * the player that calls the ioctl
 */
int mvc3d_sysfs_write(const char *path, const char *val)
{
	struct kw k = { .path = path, .val = val };
	char mod[32];
	const char *name;

	if (mvc3d_kparam_path(path, mod, sizeof(mod), &name)) {
		k.ret = mvc3d_kparam(mod, name, val, NULL, 0);
		if (k.ret)
			pr_warn("%s <- %s: %d\n", path, val, k.ret);
		return k.ret;
	}
	INIT_WORK_ONSTACK(&k.work, kw_fn);
	schedule_work(&k.work);
	flush_work(&k.work);
	destroy_work_on_stack(&k.work);
	if (k.ret)
		pr_warn("%s <- %s: %d\n", path, val, k.ret);
	return k.ret;
}

/*
 * Anything that opens a file runs in a kernel worker: the ioctl caller may be a process in its exit
 * (hw_release from do_exit has no fs context any more: filp_open there oopsed, 11-05)
 */
struct kcall {
	struct work_struct work;
	void (*fn)(void *arg);
	void *arg;
};

static void kcall_fn(struct work_struct *work)
{
	struct kcall *k = container_of(work, struct kcall, work);

	k->fn(k->arg);
}

void mvc3d_in_worker(void (*fn)(void *), void *arg)
{
	struct kcall k = { .fn = fn, .arg = arg };

	INIT_WORK_ONSTACK(&k.work, kcall_fn);
	schedule_work(&k.work);
	flush_work(&k.work);
	destroy_work_on_stack(&k.work);
}

/*
 * the chain of the map "default" from /sys/class/vfm/map: "[00]  default { decoder(0) ppmgr(0)
 * amvideo}"
 */
static void read_default(struct sc2_dev *h)
{
	struct file *f = filp_open("/sys/class/vfm/map", O_RDONLY, 0);
	char *buf, *l, *e, *o = h->default_chain;
	loff_t pos = 0;
	ssize_t n;

	strscpy(h->default_chain, "decoder ppmgr deinterlace amvideo", sizeof(h->default_chain));
	if (IS_ERR(f))
		return;
	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf) {
		filp_close(f, NULL);
		return;
	}
	n = kernel_read(f, buf, PAGE_SIZE - 1, &pos);
	filp_close(f, NULL);
	l = n > 0 ? strstr(buf, " default {") : NULL;
	e = l ? strchr(l, '}') : NULL;
	if (e) {
		char *p = strchr(l, '{') + 1;
		size_t left = sizeof(h->default_chain) - 1;

		*e = 0;
		while (*p && left) {
			if (*p == '(') {
				while (*p && *p != ')')
					p++;
				if (*p)
					p++;
				continue;
			}
			if (*p != ' ' || (o > h->default_chain && o[-1] != ' ')) {
				*o++ = *p;
				left--;
			}
			p++;
		}
		while (o > h->default_chain && o[-1] == ' ')
			o--;
		*o = 0;
		if (!h->default_chain[0])
			strscpy(h->default_chain, "decoder ppmgr deinterlace amvideo",
				sizeof(h->default_chain));
	}
	kfree(buf);
}

/*
 * "decoder.h264mvc" matches "default" by its prefix and the first map wins: "default" is off
 * meanwhile
 */
static int route(struct sc2_dev *h, bool on)
{
	char id[32], chain[160];

	if (on == h->routed)
		return 0;
	if (on) {
		read_default(h);
		strscpy(id, "default", sizeof(id));
		vfm_map_remove(id);
		strscpy(id, HW_MAP_ID, sizeof(id));
		strscpy(chain, HW_MAP_CHAIN, sizeof(chain));
		vfm_map_remove(id);
		if (vfm_map_add(id, chain) < 0) {
			pr_err("cannot add vfm map %s\n", id);
			strscpy(id, "default", sizeof(id));
			strscpy(chain, h->default_chain, sizeof(chain));
			vfm_map_add(id, chain);
			return -EIO;
		}
	} else {
		strscpy(id, HW_MAP_ID, sizeof(id));
		vfm_map_remove(id);
		strscpy(id, "default", sizeof(id));
		strscpy(chain, h->default_chain, sizeof(chain));
		if (vfm_map_add(id, chain) < 0)
			pr_err("cannot restore vfm map default (%s)\n", chain);
	}
	h->routed = on;
	pr_info("%s routed %s\n", HW_PROVIDER,
		on ? "through " HW_NAME " to amvideo" : "back to default");
	return 0;
}

/* ------------------------------------------------------------------ */
/* slots (h->lock held)                                               */
/* ------------------------------------------------------------------ */

static int slot_of_locked(struct sc2_dev *h, struct vframe_s *vf)
{
	int i;

	for (i = 0; i < NSLOT; i++)
		if (h->slots[i].state != S_FREE && h->slots[i].vf == vf)
			return i;
	return -1;
}

static int free_slot_locked(struct sc2_dev *h)
{
	int i;

	for (i = 0; i < NSLOT; i++)
		if (h->slots[i].state == S_FREE)
			return i;
	return -1;
}

/*
 * debug=1: hardware-level tear detectors: at each picture given back to the decoder, and each
 * picture got from it, its left/right luma address against what VD1/VD2 scan at that moment (their
 * address registers) - with the encoder line and the time in the kernel log (a decoder writing into
 * the picture being scanned tears the left eye's top). Puts of a flush (seek, stop) are counted
 * apart. Saved by the session script with the rest (last_session_dmesg.txt).
 */
int (*mvc3d_read_vcbus)(unsigned int reg);
static u32 (*p_enc_line)(void);
static u64 det_put_scan[2], det_put_scan_flush[2], det_get_scan[2], det_logged;
static bool det_flush; /* drop_all: the puts are a flush */

static void det_check_locked(struct sc2_dev *h, struct slot *s, bool put)
{
	u64 vd1, vd2;
	bool l, r;

	if (!READ_ONCE(mvc3d_debug) || !mvc3d_read_vcbus || !READ_ONCE(mvc3d_fp_on) || !h->fp ||
	    !s->addr_l)
		return;
	vd1 = (u64)(u32)mvc3d_read_vcbus(0x4820) << 4;
	vd2 = (u64)(u32)mvc3d_read_vcbus(0x48a0) << 4;
	l = s->addr_l == vd1;
	r = s->addr_r && s->addr_r == vd2;
	if (!l && !r)
		return;
	if (put && det_flush) {
		det_put_scan_flush[0] += l;
		det_put_scan_flush[1] += r;
		return;
	}
	if (put) {
		det_put_scan[0] += l;
		det_put_scan[1] += r;
	} else {
		det_get_scan[0] += l;
		det_get_scan[1] += r;
	}
	if (det_logged < 80) {
		det_logged++;
		pr_info("%s picture %u %s%s%s scanned now (line %u, vsync %u, state %c)\n",
			put ? "PUT-ON-SCREEN: back to the decoder:" :
			      "DECODED-ON-SCREEN: from the decoder:",
			s->seq, l ? "left" : "", l && r ? "+" : "", r ? "right" : "",
			p_enc_line ? p_enc_line() : 0xffff, h->vs_seq, "-RCQSXD"[s->state]);
	}
}

/*
 * the slot's picture back to the decoder now, in whatever order (it may be called from the vsync
 * interrupt)
 */
static void slot_put_now_locked(struct sc2_dev *h, int i)
{
	struct slot *s = &h->slots[i];

	if (h->prov_reg && s->vf) {
		u64 now = ktime_get_ns();

		det_check_locked(h, s, true);

		rh_note_locked(s->addr_l, 0, now);
		rh_note_locked(s->addr_r, 1, now);
		vf_put(s->vf, HW_NAME);
		h->put_back++;
	}
	s->vf = NULL;
	s->addr_l = s->addr_r = 0;
	s->state = S_FREE;
}

/*
 * (h->lock held) a picture just got from the decoder: its buffers still ours, or back how long ago
 */
struct reuse_note {
	int n;
	int view[2];
	int state[2];
	u32 seq_new, seq_old[2];
};
static void reuse_check_locked(struct sc2_dev *h, int i, u64 now, struct reuse_note *rn)
{
	int v, j;

	rn->n = 0;
	if (!READ_ONCE(mvc3d_debug))
		return;
	for (v = 0; v < 2; v++) {
		u64 a = v ? h->slots[i].addr_r : h->slots[i].addr_l;
		u32 k;

		if (!a)
			continue;
		for (j = 0; j < NSLOT; j++) {
			if (j == i || h->slots[j].state == S_FREE)
				continue;
			if ((v ? h->slots[j].addr_r : h->slots[j].addr_l) == a) {
				reuse_held[v]++;
				if (rn->n < 2) {
					rn->view[rn->n] = v;
					rn->state[rn->n] = h->slots[j].state;
					rn->seq_old[rn->n] = h->slots[j].seq;
					rn->n++;
				}
			}
		}
		for (k = 0; k < RH_N && k < rh_head; k++) {
			struct rh_rec *r = &rh_ring[(rh_head - 1 - k) % RH_N];

			if (r->addr == a && r->view == v) {
				u64 us = div_u64(now - r->ns, 1000);
				int b = us < 42000 ? 0 : us < 84000 ? 1 : us < 168000 ? 2 : 3;

				reuse_dt[v][b]++;
				if (us < reuse_min_us[v])
					reuse_min_us[v] = us;
				break;
			}
		}
	}
	rn->seq_new = h->slots[i].seq;
}

/* the slot holding picture seq, -1: none (put back already) */
static int slot_of_seq_locked(struct sc2_dev *h, u32 seq)
{
	int i;

	for (i = 0; i < NSLOT; i++)
		if (h->slots[i].state != S_FREE && h->slots[i].vf && h->slots[i].seq == seq)
			return i;
	return -1;
}

/* the S_DONE pictures whose older ones are all back: to the decoder, oldest first */
static void flush_puts_locked(struct sc2_dev *h)
{
	int n, i, done = 0, guard;

	for (guard = 0; guard < 4 * NSLOT && h->put_next != h->seq; guard++) {
		i = slot_of_seq_locked(h, h->put_next);
		if (i >= 0 && h->slots[i].state != S_DONE)
			break;
		if (i >= 0)
			slot_put_now_locked(h, i);
		h->put_next++;
	}
	if (h->put_next == h->seq)
		return;
	/*
	 * a picture held long (the display's, a client's) blocks the newer: the decoder must not
	 * run short of buffers - more than NSLOT / 2 waiting: the oldest of them goes now (the old,
	 * unordered way)
	 */
	for (n = 0; n < NSLOT; n++)
		if (h->slots[n].state == S_DONE)
			done++;
	while (done > NSLOT / 2) {
		int o = -1;

		for (n = 0; n < NSLOT; n++)
			if (h->slots[n].state == S_DONE &&
			    (o < 0 || (s32)(h->slots[n].seq - h->slots[o].seq) < 0))
				o = n;
		if (o < 0)
			break;
		slot_put_now_locked(h, o);
		put_forced++;
		done--;
	}
}

/*
 * the slot's picture back to the decoder once the older ones are (it may be called from the vsync
 * interrupt)
 */
static void slot_put_locked(struct sc2_dev *h, int i)
{
	struct slot *s = &h->slots[i];

	if (!h->prov_reg || !s->vf) {
		slot_put_now_locked(h, i);
		flush_puts_locked(h);
		return;
	}
	if (s->seq != h->put_next)
		put_held++;
	s->state = S_DONE;
	flush_puts_locked(h);
}

/*
 * all pictures not on the display back to the decoder; shown ones come back by amvideo's put.
 * keep_retired: pictures amvideo has just given back (S_RETIRED, maybe still scanned out) wait for
 * their put_delay_vsync as in play (a seek's flush); else they go now too (stop, decoder reset).
 */
static void drop_all_locked(struct sc2_dev *h, bool keep_retired)
{
	int i;

	/* a flush: the decoder needs its buffers for the next pictures */
	det_flush = true;
	for (i = 0; i < NSLOT; i++)
		if (h->slots[i].state == S_READY || h->slots[i].state == S_CLIENT ||
		    h->slots[i].state == S_QUEUED || h->slots[i].state == S_DONE ||
		    (h->slots[i].state == S_RETIRED && !keep_retired))
			slot_put_now_locked(h, i);
	det_flush = false;
	h->ready_cnt = h->disp_cnt = 0;
	h->ready_head = h->disp_head = 0;
	flush_puts_locked(h);
}

/* the decoder is gone: its frames are invalid, nothing is put back */
static void forget_all_locked(struct sc2_dev *h)
{
	int i;

	for (i = 0; i < NSLOT; i++) {
		h->slots[i].vf = NULL;
		h->slots[i].state = S_FREE;
	}
	h->ready_cnt = h->disp_cnt = 0;
	h->ready_head = h->disp_head = 0;
	h->put_next = h->seq;
}

/* ------------------------------------------------------------------ */
/* the decoder side: pull its pictures into slots                     */
/* ------------------------------------------------------------------ */

static void kick(struct sc2_dev *h)
{
	h->kick = true;
	wake_up_interruptible(&h->wq_thread);
}

static int pull_thread(void *data)
{
	struct sc2_dev *h = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible_timeout(h->wq_thread, h->kick || kthread_should_stop(),
						 msecs_to_jiffies(50));
		h->kick = false;
		for (;;) {
			struct vframe_s *vf = NULL;
			unsigned long flags;
			bool got = false;
			int i;

			struct reuse_note rn = { 0 };

			spin_lock_irqsave(&h->lock, flags);
			i = h->prov_reg && h->started ? free_slot_locked(h) : -1;
			if (i >= 0 && vf_peek(HW_NAME)) {
				vf = vf_get(HW_NAME);
				if (vf) {
					struct slot *s = &h->slots[i];

					s->vf = vf;
					s->state = S_READY;
					s->seq = h->seq++;
					s->gen = h->gen;
					s->decoded_ns = ktime_get_ns();
					s->addr_l = vf->canvas0_config[0].phy_addr;
					s->addr_r = vf->canvas1_config[0].phy_addr;
					reuse_check_locked(h, i, s->decoded_ns, &rn);
					det_check_locked(h, s, false);
					h->ready[(h->ready_head + h->ready_cnt) % NSLOT] = i;
					h->ready_cnt++;
					h->pics_in++;
					got = true;
				}
			}
			spin_unlock_irqrestore(&h->lock, flags);
			if (rn.n && reuse_logged < 20) {
				int k;

				reuse_logged++;
				for (k = 0; k < rn.n; k++)
					pr_info("decoder reused the %s buffer of picture %u (state %c) for picture %u\n",
						rn.view[k] ? "right" : "left", rn.seq_old[k],
						"-RCQSXD"[rn.state[k]], rn.seq_new);
			}
			if (!got)
				break;
			wake_up_interruptible(&h->wq_client);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* the display side: provider "mvc3dhw" for amvideo                   */
/* ------------------------------------------------------------------ */

/*
 * The right eye of a frame packed picture is VD2 reading the right view through its plain MIF, set
 * up by amvideo as part of VD1 (vd1_set_dcu: PSEL 0, VD1 left, VD2 right). Which input VD2 shows -
 * its MIF or its AFBC decoder - is bits 13-14 of VD2_AFBCDM_VDTOP_CTRL0 (T7 layout), and amvideo
 * sets them only when VD2 shows a picture of its own (vd2_set_dcu). After a compressed picture on
 * VD2 - Dolby Vision FEL shows its enhancement layer there - they stay on AFBC, and the right eye is
 * the idle decoder's default colour: black (seen 2026-11-26: 0x7200 after a FEL title, 0x1200 on a
 * fresh boot; clearing the bits by hand brought the right eye back). So while frame packed, VD2's
 * input is kept on its MIF. Not put back at the end: the MIF is the boot state, and amvideo sets the
 * bits again for VD2's next own picture.
 */
#define VD2_AFBCDM_VDTOP_CTRL0 0x48b8
#define VDTOP_INPUT_AFBC (3U << 13)
static u64 vd2_mif_kept;
static u32 vd2_mif_first;

/* (h->lock held) in amvideo's vsync interrupt */
static void vd2_mif_keep_locked(struct sc2_dev *h)
{
	int v;

	if (!h->started || !h->fp || !mvc3d_read_vcbus)
		return;
	v = mvc3d_read_vcbus(VD2_AFBCDM_VDTOP_CTRL0);
	if (v == -1 || !(v & VDTOP_INPUT_AFBC))
		return;
	aml_vcbus_update_bits(VD2_AFBCDM_VDTOP_CTRL0, VDTOP_INPUT_AFBC, 0);
	if (!vd2_mif_kept++) {
		vd2_mif_first = v;
		pr_info("right eye: VD2 input back to its MIF (VD2_AFBCDM_VDTOP_CTRL0 %#x, AFBC left on by an earlier picture)\n",
			v);
	}
}

/* (h->lock held) in amvideo's vsync interrupt: the vsync period */
static void dg_vsync_locked(struct sc2_dev *h);
static void vs_tick_locked(struct sc2_dev *h, u64 now)
{
	u64 dt;

	if (!in_irq())
		return;
	dt = now - h->vs_last_ns;
	if (h->vs_last_ns && dt > 3 * NSEC_PER_MSEC && dt < 100 * NSEC_PER_MSEC) {
		u64 p = h->vs_period_ns;

		if (!p) {
			h->vs_period_ns = dt;
		} else if (dt > p - p / 8 && dt < p + p / 8) {
			h->vs_period_ns = (p * 7 + dt) / 8;
			h->vs_odd = 0;
		} else if (h->vs_odd && dt > h->vs_odd_ns - h->vs_odd_ns / 8 &&
			   dt < h->vs_odd_ns + h->vs_odd_ns / 8) {
			/*
			 * another refresh rate (the mode changed): 6 like intervals in a row; a
			 * missed vsync is one
			 */
			if (++h->vs_odd >= 6) {
				h->vs_period_ns = dt;
				h->vs_odd = 0;
			}
		} else {
			h->vs_odd_ns = dt;
			h->vs_odd = 1;
		}
	}
	if (!h->vs_last_ns || dt > 3 * NSEC_PER_MSEC) {
		int i;

		h->vs_last_ns = now;
		h->vs_seq++;
		vd2_mif_keep_locked(h);
		dg_vsync_locked(h);
		/*
		 * pictures back from the display 2 (+ put_delay_vsync) vsyncs ago: the VPP is done
		 * with them
		 */
		for (i = 0; i < NSLOT; i++)
			if (h->slots[i].state == S_RETIRED &&
			    h->vs_seq - h->slots[i].retire_seq >=
				    2 + (u32)clamp(READ_ONCE(put_delay_vsync), 0, 4))
				slot_put_locked(h, i);
	}
}

/* ------------------------------------------------------------------ */
/* Diagnostics: which picture each video layer scans out  */
/* ------------------------------------------------------------------ */
/*
 * In amvideo's vsync interrupt (our first peek/get of a vsync): the encoder line, and the luma
 * addresses VD1 (left: VD1_IF0_BADDR_Y) and VD2 (right: VD2_IF0_BADDR_Y) scan from (T7 linear MIF),
 * plus the canvas registers (older layout). Each address is matched to the slot whose left/right
 * view it is: a vsync where the left is picture n and the right picture m != n is a split (the eyes
 * from different times). The get records the encoder line at the swap. Read only (MMIO reads); with
 * debug=1 only.
 */
static u64 dg_split_logged, dg_unknown_logged;
#define DG_N 256
struct dg_rec {
	u32 vs; /* vsync seq */
	u16 line; /* encoder line at the vsync interrupt */
	u16 get_line; /* encoder line at the get (0xffff: none this vsync) */
	s8 l_slot, r_slot; /* slot of VD1's / VD2's picture (-1 unknown) */
	s8 got; /* slot taken this vsync */
	u32 l_seq, r_seq; /* their decode order */
	u32 vd1y, vd2y, vd1y1, vd2y1, c1, c2;
};
static struct dg_rec dg_ring[DG_N];
static u32 dg_head;
static u64 dg_vs, dg_same, dg_split, dg_unknown, dg_split_lag[4];
static int dg_last; /* index of this vsync's record */

static int dg_slot_of(struct sc2_dev *h, u64 a, bool right)
{
	int i;

	if (!a)
		return -1;
	for (i = 0; i < NSLOT; i++)
		if (h->slots[i].vf && (right ? h->slots[i].addr_r : h->slots[i].addr_l) == a)
			return i;
	return -1;
}

/* (h->lock held, in the vsync interrupt, once per vsync) */
static void dg_vsync_locked(struct sc2_dev *h)
{
	struct dg_rec *r;

	if (!READ_ONCE(mvc3d_debug) || !mvc3d_read_vcbus || !h->started)
		return;
	r = &dg_ring[dg_head % DG_N];
	dg_last = dg_head % DG_N;
	dg_head++;
	memset(r, 0, sizeof(*r));
	r->vs = h->vs_seq;
	r->line = p_enc_line ? p_enc_line() : 0xffff;
	if (r->line != 0xffff && r->line > 41 && h->fp)
		isr_late++;
	r->get_line = 0xffff;
	r->got = -1;
	r->vd1y = mvc3d_read_vcbus(0x4820);
	r->vd1y1 = mvc3d_read_vcbus(0x4825);
	r->vd2y = mvc3d_read_vcbus(0x48a0);
	r->vd2y1 = mvc3d_read_vcbus(0x48a5);
	r->c1 = mvc3d_read_vcbus(0x1a51);
	r->c2 = mvc3d_read_vcbus(0x1a71);
	r->l_slot = dg_slot_of(h, (u64)r->vd1y << 4, false);
	r->r_slot = dg_slot_of(h, (u64)r->vd2y << 4, true);
	r->l_seq = r->l_slot >= 0 ? h->slots[r->l_slot].seq : 0;
	r->r_seq = r->r_slot >= 0 ? h->slots[r->r_slot].seq : 0;
	if (!h->fp)
		return;
	dg_vs++;
	if (r->l_slot < 0 || r->r_slot < 0) {
		dg_unknown++;
		if (dg_unknown_logged < 40) {
			dg_unknown_logged++;
			pr_info("EYES-UNKNOWN vsync %u line %u: left %08x (slot %d) right %08x (slot %d)\n",
				r->vs, r->line, r->vd1y, r->l_slot, r->vd2y, r->r_slot);
		}
	} else if (r->l_slot == r->r_slot) {
		dg_same++;
	} else {
		s32 d = (s32)(r->l_seq - r->r_seq);

		dg_split++;
		dg_split_lag[d == 1 ? 0 : d == -1 ? 1 : d > 0 ? 2 : 3]++;
		if (dg_split_logged < 100) {
			dg_split_logged++;
			pr_info("EYES-SPLIT vsync %u line %u: left picture %u right picture %u (%+d)\n",
				r->vs, r->line, r->l_seq, r->r_seq, d);
		}
	}
}

static void dg_get_locked(struct sc2_dev *h, int slot)
{
	struct vframe_s *vf = h->slots[slot].vf;

	if (vf) {
		h->slots[slot].addr_l = vf->canvas0_config[0].phy_addr;
		h->slots[slot].addr_r = vf->canvas1_config[0].phy_addr;
	}
	if (!READ_ONCE(mvc3d_debug) || !mvc3d_read_vcbus || dg_head == 0)
		return;
	dg_ring[dg_last].got = slot;
	dg_ring[dg_last].get_line = p_enc_line ? p_enc_line() : 0xffff;
}

static int vsynclog_show(struct seq_file *m, void *v)
{
	struct sc2_dev *h = mvc3d_dev;
	unsigned long flags;
	static struct dg_rec copy[DG_N];
	u32 head, n, i;
	u64 vs, same, split, unk, lag[4];

	spin_lock_irqsave(&h->lock, flags);
	memcpy(copy, dg_ring, sizeof(copy));
	head = dg_head;
	vs = dg_vs;
	same = dg_same;
	split = dg_split;
	unk = dg_unknown;
	memcpy(lag, dg_split_lag, sizeof(lag));
	spin_unlock_irqrestore(&h->lock, flags);
	seq_printf(
		m,
		"frame packed vsyncs %llu: both eyes one picture %llu, split %llu (left newer by 1: %llu, right newer by 1: %llu, left newer >1: %llu, right newer >1: %llu), unknown %llu\n",
		vs, same, split, lag[0], lag[1], lag[2], lag[3], unk);
	seq_printf(m, "vsync irq after line 41: %llu\n", isr_late);
	seq_printf(
		m,
		"decoder reuse of held buffers: left %llu right %llu; back after <1/<2/<4/>=4 frames: left %llu/%llu/%llu/%llu right %llu/%llu/%llu/%llu, min left %lld us right %lld us\n",
		reuse_held[0], reuse_held[1], reuse_dt[0][0], reuse_dt[0][1], reuse_dt[0][2],
		reuse_dt[0][3], reuse_dt[1][0], reuse_dt[1][1], reuse_dt[1][2], reuse_dt[1][3],
		reuse_min_us[0] == ~0ULL ? -1LL : (s64)reuse_min_us[0],
		reuse_min_us[1] == ~0ULL ? -1LL : (s64)reuse_min_us[1]);
	seq_puts(
		m,
		"vsync line get_line got | L slot/seq VD1_Y VD1_Y_F1 | R slot/seq VD2_Y VD2_Y_F1 | canvas VD1 VD2\n");
	n = head < DG_N ? head : DG_N;
	for (i = head - n; i != head; i++) {
		struct dg_rec *r = &copy[i % DG_N];

		seq_printf(m, "%u %u %d %d | %d/%u %08x %08x | %d/%u %08x %08x | %08x %08x%s\n",
			   r->vs, r->line, r->get_line == 0xffff ? -1 : r->get_line, r->got,
			   r->l_slot, r->l_seq, r->vd1y, r->vd1y1, r->r_slot, r->r_seq, r->vd2y,
			   r->vd2y1, r->c1, r->c2,
			   r->l_slot >= 0 && r->r_slot >= 0 && r->l_slot != r->r_slot ? "  SPLIT" :
											"");
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vsynclog);

/* debug=1: the session's diagnostics summary in the kernel log (kept by the session script) */
static void diag_summary(void)
{
	pr_info("diag vsyncs %llu same %llu split %llu (lag +1 %llu, -1 %llu, >1 %llu, <-1 %llu) unknown %llu, re-enables %u, vsync irq late %llu\n",
		dg_vs, dg_same, dg_split, dg_split_lag[0], dg_split_lag[1], dg_split_lag[2],
		dg_split_lag[3], dg_unknown, reenables, isr_late);
	pr_info("scanned-buffer detectors: put while scanned left %llu right %llu (flush %llu/%llu), got while scanned left %llu right %llu; put delay %d vsyncs\n",
		det_put_scan[0], det_put_scan[1], det_put_scan_flush[0], det_put_scan_flush[1],
		det_get_scan[0], det_get_scan[1], 2 + READ_ONCE(put_delay_vsync));
	pr_info("reuse held left %llu right %llu; back <1/<2/<4/>=4 frames left %llu/%llu/%llu/%llu right %llu/%llu/%llu/%llu; min us left %lld right %lld\n",
		reuse_held[0], reuse_held[1], reuse_dt[0][0], reuse_dt[0][1], reuse_dt[0][2],
		reuse_dt[0][3], reuse_dt[1][0], reuse_dt[1][1], reuse_dt[1][2], reuse_dt[1][3],
		reuse_min_us[0] == ~0ULL ? -1LL : (s64)reuse_min_us[0],
		reuse_min_us[1] == ~0ULL ? -1LL : (s64)reuse_min_us[1]);
}

/*
 * (h->lock held) a session's counters and log caps start at 0: the module may stay loaded from one
 * session to the next (the session script keeps a loaded module of the same version)
 */
static void session_stats_reset_locked(void)
{
	put_held = 0;
	put_forced = 0;
	reenables = 0;
	isr_late = 0;
	rh_head = 0;
	memset(reuse_held, 0, sizeof(reuse_held));
	memset(reuse_dt, 0, sizeof(reuse_dt));
	reuse_min_us[0] = ~0ULL;
	reuse_min_us[1] = ~0ULL;
	reuse_logged = 0;
	memset(det_put_scan, 0, sizeof(det_put_scan));
	memset(det_put_scan_flush, 0, sizeof(det_put_scan_flush));
	memset(det_get_scan, 0, sizeof(det_get_scan));
	det_logged = 0;
	dg_head = 0;
	dg_last = 0;
	dg_vs = 0;
	dg_same = 0;
	dg_split = 0;
	dg_unknown = 0;
	memset(dg_split_lag, 0, sizeof(dg_split_lag));
	dg_split_logged = 0;
	dg_unknown_logged = 0;
}

/* a picture is shown from the first vsync after its due time - half a vsync */
static u64 lead_ns_locked(struct sc2_dev *h)
{
	return h->vs_period_ns ? h->vs_period_ns / 2 : 8 * NSEC_PER_MSEC;
}

/*
 * (h->lock held) the picture to show now: the head of the display queue once due; when the next one
 * is due too, the head is late: back to the decoder
 */
static struct slot *due_locked(struct sc2_dev *h, u64 now)
{
	u64 lead = lead_ns_locked(h);

	while (h->disp_cnt) {
		struct slot *s = &h->slots[h->dispq[h->disp_head]];

		if (s->due_ns > now + lead)
			return NULL;
		if (h->disp_cnt > 1) {
			struct slot *n = &h->slots[h->dispq[(h->disp_head + 1) % NSLOT]];

			if (n->due_ns <= now + lead) {
				slot_put_locked(h, h->dispq[h->disp_head]);
				h->disp_head = (h->disp_head + 1) % NSLOT;
				h->disp_cnt--;
				h->dropped_late++;
				continue;
			}
		}
		return s;
	}
	return NULL;
}

static struct vframe_s *sc2_peek(void *op_arg)
{
	struct sc2_dev *h = op_arg;
	struct vframe_s *vf = NULL;
	unsigned long flags;
	struct slot *s;
	u64 now = ktime_get_ns();

	spin_lock_irqsave(&h->lock, flags);
	vs_tick_locked(h, now);
	s = h->prov_reg ? due_locked(h, now) : NULL;
	if (s)
		vf = s->vf;
	spin_unlock_irqrestore(&h->lock, flags);
	return vf;
}

static struct vframe_s *sc2_get(void *op_arg)
{
	struct sc2_dev *h = op_arg;
	struct vframe_s *vf = NULL;
	unsigned long flags;
	struct slot *s;
	u64 now = ktime_get_ns();

	spin_lock_irqsave(&h->lock, flags);
	vs_tick_locked(h, now);
	s = h->prov_reg ? due_locked(h, now) : NULL;
	if (s) {
		s64 late = (s64)(now - s->due_ns) / 1000;

		vf = s->vf;
		s->state = S_SHOWN;
		dg_get_locked(h, s - h->slots);
		if (READ_ONCE(mvc3d_fp_on))
			mvc3d_fp_conv_vf(vf);
		h->disp_head = (h->disp_head + 1) % NSLOT;
		h->disp_cnt--;
		h->shown++;
		h->late_us_sum += late;
		h->late_cnt++;
		if (late > h->late_max_us)
			h->late_max_us = late;
	}
	spin_unlock_irqrestore(&h->lock, flags);
	return vf;
}

static void sc2_put(struct vframe_s *vf, void *op_arg)
{
	struct sc2_dev *h = op_arg;
	unsigned long flags;
	int i;

	if (!vf)
		return;
	spin_lock_irqsave(&h->lock, flags);
	i = slot_of_locked(h, vf);
	if (i >= 0 && h->slots[i].state == S_SHOWN && h->prov_reg) {
		/*
		 * amvideo gives it back when it switches to the next picture, which the VPP takes
		 * from the next vsync on: the decoder (short of free buffers while the player holds
		 * some) would write into it at once - a torn picture. It goes back 2 + put_delay_vsync
		 * vsyncs later (vs_tick_locked).
		 */
		h->slots[i].state = S_RETIRED;
		h->slots[i].retire_seq = h->vs_seq;
	} else if (i >= 0) {
		slot_put_locked(h, i);
	} else {
		/* not ours (any more): to the decoder if it is still there */
		h->stale_put++;
		if (h->prov_reg)
			vf_put(vf, HW_NAME);
	}
	spin_unlock_irqrestore(&h->lock, flags);
	kick(h);
}

static int sc2_prov_event(int type, void *data, void *op_arg)
{
	vf_notify_provider(HW_NAME, type, data);
	return 0;
}

static int sc2_vf_states(struct vframe_states *st, void *op_arg)
{
	struct sc2_dev *h = op_arg;
	unsigned long flags;
	int i, nfree = 0;

	spin_lock_irqsave(&h->lock, flags);
	for (i = 0; i < NSLOT; i++)
		if (h->slots[i].state == S_FREE)
			nfree++;
	st->vf_pool_size = NSLOT;
	st->buf_free_num = nfree;
	st->buf_recycle_num = 0;
	st->buf_avail_num = h->disp_cnt;
	spin_unlock_irqrestore(&h->lock, flags);
	return 0;
}

static const struct vframe_operations_s sc2_vf_ops = {
	.peek = sc2_peek,
	.get = sc2_get,
	.put = sc2_put,
	.event_cb = sc2_prov_event,
	.vf_states = sc2_vf_states,
};

/* ------------------------------------------------------------------ */
/* receiver side (decoder events)                                     */
/* ------------------------------------------------------------------ */

static int sc2_recv_event(int type, void *data, void *op_arg)
{
	struct sc2_dev *h = op_arg;
	unsigned long flags;

	switch (type) {
	case VFRAME_EVENT_PROVIDER_REG:
		spin_lock_irqsave(&h->lock, flags);
		forget_all_locked(h);
		h->prov_reg = true;
		h->seq = 0;
		h->put_next = 0;
		spin_unlock_irqrestore(&h->lock, flags);
		pr_info("decoder registered\n");
		vf_reg_provider(&h->prov);
		if (READ_ONCE(h->started) && p_video_set_disable) {
			int r = p_video_set_disable(0);

			reenables++;
			pr_info("video layer re-enabled for the new decoder (disable_video 0: %d)\n",
				r);
		}
		kick(h);
		break;
	case VFRAME_EVENT_PROVIDER_UNREG:
		spin_lock_irqsave(&h->lock, flags);
		h->prov_reg = false;
		forget_all_locked(h);
		spin_unlock_irqrestore(&h->lock, flags);
		/* amvideo keeps a copy of what it shows */
		vf_unreg_provider(&h->prov);
		mvc3d_pool_decoder_gone();
		pr_info("decoder unregistered after %u pictures\n", h->seq);
		wake_up_interruptible(&h->wq_client);
		break;
	case VFRAME_EVENT_PROVIDER_LIGHT_UNREG:
		spin_lock_irqsave(&h->lock, flags);
		drop_all_locked(h, false);
		spin_unlock_irqrestore(&h->lock, flags);
		vf_light_unreg_provider(&h->prov);
		wake_up_interruptible(&h->wq_client);
		break;
	case VFRAME_EVENT_PROVIDER_RESET:
		spin_lock_irqsave(&h->lock, flags);
		drop_all_locked(h, false);
		spin_unlock_irqrestore(&h->lock, flags);
		vf_notify_receiver(HW_NAME, type, data);
		wake_up_interruptible(&h->wq_client);
		break;
	case VFRAME_EVENT_PROVIDER_VFRAME_READY:
		kick(h);
		break;
	case VFRAME_EVENT_PROVIDER_QUREY_STATE:
		return RECEIVER_ACTIVE;
	default:
		return vf_notify_receiver(HW_NAME, type, data);
	}
	return 0;
}

static const struct vframe_receiver_op_s sc2_recv_ops = {
	.event_cb = sc2_recv_event,
};

/* ------------------------------------------------------------------ */
/* /dev/mvc3dhw                                                       */
/* ------------------------------------------------------------------ */

/*
 * A client gone before its decoder (killed: this file closes before its amstream): the decoder's
 * end comes through our route only, so the route stays a moment for it - without it the
 * "registered" decoder stayed for ever and every later start was refused (11-06). A decoder still
 * there after that is forgotten.
 */
static void route_off_fn(struct work_struct *w)
{
	struct sc2_dev *h = mvc3d_dev;
	unsigned long flags;
	bool stale;

	mutex_lock(&h->mlock);
	if (!h->started) {
		spin_lock_irqsave(&h->lock, flags);
		stale = h->prov_reg;
		h->prov_reg = false;
		if (stale)
			forget_all_locked(h);
		spin_unlock_irqrestore(&h->lock, flags);
		if (stale) {
			pr_warn("a decoder without a client: forgotten\n");
			vf_unreg_provider(&h->prov);
			mvc3d_pool_decoder_gone();
		}
		route(h, false);
	}
	mutex_unlock(&h->mlock);
}
static DECLARE_DELAYED_WORK(route_work, route_off_fn);

static void hw_stop(struct sc2_dev *h)
{
	unsigned long flags;
	bool decoder, fp = h->fp;

	spin_lock_irqsave(&h->lock, flags);
	h->started = false;
	drop_all_locked(h, false);
	decoder = h->prov_reg;
	spin_unlock_irqrestore(&h->lock, flags);
	if (decoder)
		schedule_delayed_work(&route_work, HZ);
	else
		route(h, false);
	set_freerun_mode(0);
	mvc3d_sysfs_write(MVC_PARAMS "view_mode", "3");
	if (fp) {
		mvc3d_sysfs_write(FP_PARAM, "0");
		mvc3d_fp_3d_type(0);
		mvc3d_fp_arm(false);
	}
	h->fp = false;
	pr_info("stop (in %llu, shown %llu, late drops %llu, client drops %llu)\n", h->pics_in,
		h->shown, h->dropped_late, h->dropped_client);
	pr_info("puts held for order %llu, forced %llu\n", put_held, put_forced);
	mvc3d_fp_summary();
	if (READ_ONCE(mvc3d_debug))
		diag_summary();
	/* the display as before the session (its 2D mode set still goes through the hooks) */
	if (h->display) {
		mvc3d_display_give_back();
		h->display = false;
	}
	/* idle again: the UI no longer stretched, this session's hooks off */
	if (fp) {
		mvc3d_ui3d_want(false);
		mvc3d_fp_hooks(false);
	}
	mvc3d_pool_hooks(false);
	/*
	 * a decoder still open (the client died before its amstream) keeps the pool until it goes
	 */
	mvc3d_pool_release_if_idle(h);
}

static long ioc_start(struct sc2_dev *h, void __user *p)
{
	struct mvc3d_hw_start s;
	unsigned long flags;
	bool fp;
	int r;

	if (copy_from_user(&s, p, sizeof(s)))
		return -EFAULT;
	if (h->started)
		return -EBUSY;
	if (!s.width || s.width > 1920 || !s.height || s.height > 1088 || s.mode > MVC3D_HW_FP)
		return -EINVAL;
	fp = s.mode == MVC3D_HW_FP;
	if (fp && !mvc3d_fp_ok)
		return -ENODEV;
	/* (the last client's decoder, its process going: a moment for it) */
	for (r = 0; h->routed && r < 75 && READ_ONCE(h->prov_reg); r++)
		msleep(20);
	cancel_delayed_work(&route_work);
	if (READ_ONCE(h->prov_reg))
		return -EBUSY; /* a decoder of another player is open */
	/*
	 * this session's hooks: the decoder's allocations, for frame packing the HDMI and video
	 * paths; then its display (own_display), all undone at its stop
	 */
	r = mvc3d_pool_hooks(true);
	if (r)
		return r;
	if (fp) {
		r = mvc3d_fp_hooks(true);
		if (r)
			goto fail_pool;
		/* the UI over both eyes only for a player that draws it per eye */
		mvc3d_ui3d_want(s.reserved[1] & MVC3D_HW_F_UI3D);
		if (mvc3d_own_display()) {
			r = mvc3d_display_take();
			if (r)
				goto fail_hooks;
			h->display = true;
		}
	}
	r = need_pool ? mvc3d_pool_get(h->misc.this_device) : 0;
	if (r)
		goto fail_display;
	mvc3d_pool_reset();
	r = route(h, true);
	if (r) {
		mvc3d_pool_put();
		goto fail_display;
	}
	h->fp = fp;
	mvc3d_sysfs_write(MVC_PARAMS "view_mode", h->fp ? "3" : "0");
	mvc3d_sysfs_write(MVC_PARAMS "drop_thread_hold", "0");
	if (h->fp) {
		mvc3d_sysfs_write(FP_PARAM, "1");
		/*
		 * the VPP's frame packing of the views (as CoreELEC's Kodi: 3D on, frame
		 * alternative)
		 */
		mvc3d_fp_3d_type(MODE_3D_ENABLE | MODE_3D_FA);
		mvc3d_fp_arm(true);
	}
	/* amvideo shows what it peeks: the time is ours */
	set_freerun_mode(1);
	spin_lock_irqsave(&h->lock, flags);
	drop_all_locked(h, false);
	h->started = true;
	mvc3d_fp_session_start();
	h->gen++;
	h->pics_in = h->shown = h->dropped_late = h->dropped_client = h->released = 0;
	h->put_back = h->stale_put = h->late_us_sum = h->late_cnt = 0;
	h->late_max_us = 0;
	session_stats_reset_locked();
	s.gen = h->gen;
	spin_unlock_irqrestore(&h->lock, flags);
	s.out_height = h->fp ? 2 * s.height + (s.height == 720 ? 30 : 45) : s.height;
	s.stride = 0;
	s.reserved[0] = MVC3D_HW_F_DIRECT;
	/* the player draws its UI per eye when the UI spans both eyes */
	s.reserved[1] = h->fp && h->display && mvc3d_ui3d_active() ? MVC3D_HW_F_UI3D : 0;
	pr_info("start %ux%u %s (direct)\n", s.width, s.height, h->fp ? "frame packed" : "2D");
	kick(h);
	return copy_to_user(p, &s, sizeof(s)) ? -EFAULT : 0;

fail_display:
	if (h->display) {
		mvc3d_display_give_back();
		h->display = false;
	}
fail_hooks:
	if (fp) {
		mvc3d_ui3d_want(false);
		mvc3d_fp_hooks(false);
	}
fail_pool:
	mvc3d_pool_hooks(false);
	return r;
}

static long ioc_wait(struct sc2_dev *h, void __user *p)
{
	struct mvc3d_hw_pic w;
	unsigned long flags;
	long r;
	int i = -1;

	if (copy_from_user(&w, p, sizeof(w)))
		return -EFAULT;
	r = wait_event_interruptible_timeout(h->wq_client, READ_ONCE(h->ready_cnt) || !h->started,
					     msecs_to_jiffies(w.timeout_ms));
	if (r < 0)
		return r;
	spin_lock_irqsave(&h->lock, flags);
	if (h->ready_cnt) {
		struct slot *s;

		i = h->ready[h->ready_head];
		h->ready_head = (h->ready_head + 1) % NSLOT;
		h->ready_cnt--;
		s = &h->slots[i];
		s->state = S_CLIENT;
		w.index = i;
		w.seq = s->seq;
		w.gen = s->gen;
		w.decoded_ns = s->decoded_ns;
	}
	spin_unlock_irqrestore(&h->lock, flags);
	if (i < 0)
		return -EAGAIN;
	return copy_to_user(p, &w, sizeof(w)) ? -EFAULT : 0;
}

static long ioc_present(struct sc2_dev *h, void __user *p)
{
	struct mvc3d_hw_present q;
	unsigned long flags;
	long r = 0;

	if (copy_from_user(&q, p, sizeof(q)))
		return -EFAULT;
	spin_lock_irqsave(&h->lock, flags);
	if (q.index < 0 || q.index >= NSLOT || h->slots[q.index].state != S_CLIENT) {
		r = -EINVAL;
	} else {
		struct slot *s = &h->slots[q.index];
		int k = h->disp_cnt;

		/* in due order (the player presents in display order: almost always at the end) */
		s->due_ns = q.due_ns;
		s->state = S_QUEUED;
		while (k > 0 &&
		       h->slots[h->dispq[(h->disp_head + k - 1) % NSLOT]].due_ns > q.due_ns) {
			h->dispq[(h->disp_head + k) % NSLOT] =
				h->dispq[(h->disp_head + k - 1) % NSLOT];
			k--;
		}
		h->dispq[(h->disp_head + k) % NSLOT] = q.index;
		h->disp_cnt++;
	}
	spin_unlock_irqrestore(&h->lock, flags);
	if (!r)
		vf_notify_receiver(HW_NAME, VFRAME_EVENT_PROVIDER_VFRAME_READY, NULL);
	return r;
}

static long ioc_release(struct sc2_dev *h, void __user *p)
{
	struct mvc3d_hw_buf a;
	unsigned long flags;
	long r = 0;

	if (copy_from_user(&a, p, sizeof(a)))
		return -EFAULT;
	spin_lock_irqsave(&h->lock, flags);
	if (a.index < 0 || a.index >= NSLOT || h->slots[a.index].state != S_CLIENT) {
		r = -EINVAL;
	} else {
		slot_put_locked(h, a.index);
		h->dropped_client++;
	}
	spin_unlock_irqrestore(&h->lock, flags);
	kick(h);
	return r;
}

static long hw_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct sc2_dev *h = mvc3d_dev;
	void __user *p = (void __user *)arg;
	unsigned long flags;
	long r;

	if (cmd == MVC3D_HW_START) {
		mutex_lock(&h->mlock);
		if (!h->owner)
			h->owner = f;
		mutex_unlock(&h->mlock);
	}
	if (h->owner != f)
		return -EBUSY;
	switch (cmd) {
	case MVC3D_HW_START:
		mutex_lock(&h->mlock);
		r = ioc_start(h, p);
		mutex_unlock(&h->mlock);
		return r;
	case MVC3D_HW_ADD_BUF:
		return -EOPNOTSUPP; /* MVC3D_HW_F_DIRECT */
	case MVC3D_HW_WAIT:
		return ioc_wait(h, p);
	case MVC3D_HW_PRESENT:
		return ioc_present(h, p);
	case MVC3D_HW_RELEASE:
		return ioc_release(h, p);
	case MVC3D_HW_FLUSH:
		spin_lock_irqsave(&h->lock, flags);
		drop_all_locked(h, true);
		h->gen++;
		r = h->gen;
		spin_unlock_irqrestore(&h->lock, flags);
		kick(h);
		return put_user((u32)r, (u32 __user *)p);
	case MVC3D_HW_SET_FP: {
		u32 on;

		if (copy_from_user(&on, p, sizeof(on)))
			return -EFAULT;
		/* frame packing is armed by MVC3D_HW_START in MVC3D_HW_FP mode */
		return on && !mvc3d_fp_ok ? -ENODEV : 0;
	}
	}
	return -ENOTTY;
}

static int hw_open(struct inode *inode, struct file *f)
{
	return 0; /* the first MVC3D_HW_START makes this file the owner */
}

/*
 * The player, before its film (no ioctl from Java): write "arm" on a file of this device = frame
 * packing wanted until the film's MVC3D_HW_STOP or the file's close, so the mode set the player
 * does now (before the decoder opens) is already the frame packed one. read: "armed=N on=N
 * started=N ui3d=N\n", on = the HDMI output is frame packed, started = the hardware decoder runs (not:
 * the player has no 3D, it gives the mode back), ui3d = the session shows the Android UI over both eyes.
 */
static struct file *fp_armer;

static ssize_t hw_write(struct file *f, const char __user *u, size_t n, loff_t *pos)
{
	struct sc2_dev *h = mvc3d_dev;
	char c[8] = "";
	int r = n;

	if (copy_from_user(c, u, min(n, sizeof(c) - 1)))
		return -EFAULT;
	if (strncmp(c, "arm", 3))
		return -EINVAL;
	if (!mvc3d_fp_ok)
		return -ENODEV;
	mutex_lock(&h->mlock);
	if (fp_armer && fp_armer != f) {
		r = -EBUSY;
	} else if (!fp_armer && mvc3d_fp_hooks(true)) {
		/* the arming file holds the frame packing hooks until it is closed */
		r = -ENODEV;
	} else {
		fp_armer = f;
		mvc3d_fp_arm(true);
	}
	mutex_unlock(&h->mlock);
	return r;
}

static ssize_t hw_read(struct file *f, char __user *u, size_t n, loff_t *pos)
{
	char c[64];
	loff_t at = 0; /* always the whole line: a status */
	/* ui3d=1: the Android UI spans both eyes (rows 0-529 left eye, 551-1080 right eye) */
	int len = scnprintf(c, sizeof(c), "armed=%d on=%d started=%d ui3d=%d\n",
			    READ_ONCE(mvc3d_fp_armed), READ_ONCE(mvc3d_fp_on),
			    READ_ONCE(mvc3d_dev->started),
			    READ_ONCE(mvc3d_dev->started) && mvc3d_dev->display && mvc3d_ui3d_active());

	return simple_read_from_buffer(u, n, &at, c, len);
}

static int hw_release(struct inode *inode, struct file *f)
{
	struct sc2_dev *h = mvc3d_dev;

	mutex_lock(&h->mlock);
	if (h->owner == f) {
		if (h->started)
			hw_stop(h);
		h->owner = NULL;
	}
	if (fp_armer == f) {
		fp_armer = NULL;
		if (!h->started)
			mvc3d_fp_arm(false);
		mvc3d_fp_hooks(false);
	}
	mutex_unlock(&h->mlock);
	return 0;
}

static const struct file_operations hw_fops = {
	.owner = THIS_MODULE,
	.open = hw_open,
	.release = hw_release,
	.read = hw_read,
	.write = hw_write,
	.llseek = noop_llseek,
	.unlocked_ioctl = hw_ioctl,
	.compat_ioctl = hw_ioctl,
};

static int stats_show(struct seq_file *m, void *v)
{
	struct sc2_dev *h = mvc3d_dev;
	unsigned long flags;
	char st[NSLOT + 1];
	int i;

	spin_lock_irqsave(&h->lock, flags);
	for (i = 0; i < NSLOT; i++)
		st[i] = "-RCQSXD"[h->slots[i].state];
	st[i] = 0;
	seq_printf(m, "mvc3d %s: client %s, %s, decoder %s, route %s\n", MVC3D_MODULE_VERSION,
		   h->owner ? "yes" : "no",
		   h->started ? (h->fp ? "frame packed" : "2D") : "stopped",
		   h->prov_reg ? "registered" : "none", h->routed ? "here" : "default");
	seq_printf(
		m,
		"slots %s (Ready Client Queued Shown X-retired D-done, waiting for older), ready %d, queued %d, vsync %u us, lead %llu us\n",
		st, h->ready_cnt, h->disp_cnt, (u32)div_u64(h->vs_period_ns, 1000),
		div_u64(lead_ns_locked(h), 1000));
	seq_printf(
		m,
		"pictures in %llu (seq %u), shown %llu, late drops %llu, client drops %llu, back %llu, stale puts %llu\n",
		h->pics_in, h->seq, h->shown, h->dropped_late, h->dropped_client, h->put_back,
		h->stale_put);
	mvc3d_pool_stats(m);
	mvc3d_fp_stats(m);
	seq_printf(m, "puts held for order %llu, forced %llu\n", put_held, put_forced);
	seq_printf(m, "right eye: VD2 input back to its MIF %llu times (first from %#x)\n", vd2_mif_kept,
		   vd2_mif_first);
	seq_printf(m, "shown after due: avg %lld us, max %lld us; default map \"%s\"\n",
		   h->late_cnt ? (s64)div64_s64((s64)h->late_us_sum, h->late_cnt) : 0,
		   h->late_max_us, h->default_chain);
	spin_unlock_irqrestore(&h->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static int __init sc2_init(void)
{
	struct sc2_dev *h = kzalloc(sizeof(*h), GFP_KERNEL);
	int r;

	if (!h)
		return -ENOMEM;
	mutex_init(&h->mlock);
	spin_lock_init(&h->lock);
	init_waitqueue_head(&h->wq_client);
	init_waitqueue_head(&h->wq_thread);
	mvc3d_dev = h;
	vf_receiver_init(&h->recv, HW_NAME, &sc2_recv_ops, h);
	vf_provider_init(&h->prov, HW_NAME, &sc2_vf_ops, h);
	r = vf_reg_receiver(&h->recv);
	if (r)
		goto fail;
	h->thread = kthread_run(pull_thread, h, HW_NAME);
	if (IS_ERR(h->thread)) {
		r = PTR_ERR(h->thread);
		goto fail_recv;
	}
	h->misc.minor = MISC_DYNAMIC_MINOR;
	h->misc.name = HW_NAME;
	h->misc.fops = &hw_fops;
	/*
	 * players open it themselves: the Magisk module that loads mvc3d gives it its own SELinux type
	 * (as signde's does for /dev/dvfel); a session belongs to the file that started it
	 */
	h->misc.mode = 0666;
	r = misc_register(&h->misc);
	if (r)
		goto fail_thread;
	/* the pool's cache maintenance */
	h->misc.this_device->dma_mask = &sc2_dma_mask;
	h->misc.this_device->coherent_dma_mask = DMA_BIT_MASK(32);
	h->dbg = debugfs_create_dir(KBUILD_MODNAME, NULL);
	debugfs_create_file("hw_stats", 0444, h->dbg, NULL, &stats_fops);
	debugfs_create_file("vsynclog", 0444, h->dbg, NULL, &vsynclog_fops);
	mvc3d_read_vcbus = (void *)kallsyms_lookup_name("aml_read_vcbus");
	p_enc_line = (void *)kallsyms_lookup_name("get_cur_enc_line");
	mvc3d_pool_init();
	p_set_freerun_mode = (void *)kallsyms_lookup_name("set_freerun_mode");
	p_video_set_disable = (void *)kallsyms_lookup_name("_video_set_disable");
	if (!p_video_set_disable)
		pr_warn("_video_set_disable not found: a seek can leave the video layer off\n");
	/*
	 * max_pfn is not in kallsyms on PS7718 (no KALLSYMS_ALL): the exported high_memory instead
	 */
	ram_top = high_memory ? (unsigned long)__pa((unsigned long)high_memory - 1) + 1 : ~0UL;
	need_pool = ram_top > POOL_TOP;
	pr_info("gazelle: freerun %ps, RAM top %#lx: low DMA pool %s\n", p_set_freerun_mode,
		ram_top, need_pool ? "needed" : "not needed (all RAM below 2 GiB)");
	if (!p_set_freerun_mode) {
		r = -ENOENT;
		goto fail_probes;
	}
	if (fp_enable)
		mvc3d_fp_init();
	pr_info("%s ready (hardware MVC through amvideo)\n", MVC3D_MODULE_VERSION);
	return 0;

fail_probes:
	mvc3d_pool_exit();
	debugfs_remove_recursive(h->dbg);
	misc_deregister(&h->misc);

fail_thread:
	kthread_stop(h->thread);
fail_recv:
	vf_unreg_receiver(&h->recv);
fail:
	mvc3d_dev = NULL;
	kfree(h);
	return r;
}

static void __exit sc2_exit(void)
{
	struct sc2_dev *h = mvc3d_dev;

	mvc3d_fp_exit();
	mvc3d_pool_exit();
	debugfs_remove_recursive(h->dbg);
	misc_deregister(&h->misc);
	mutex_lock(&h->mlock);
	if (h->started)
		hw_stop(h);
	mutex_unlock(&h->mlock);
	flush_delayed_work(&route_work);
	kthread_stop(h->thread);
	if (h->prov_reg)
		vf_unreg_provider(&h->prov);
	vf_unreg_receiver(&h->recv);
	/* with a decoder still open the pool stays (it may write there) */
	if (!h->prov_reg)
		mvc3d_pool_put();
	kfree(h);
	mvc3d_dev = NULL;
}

module_init(sc2_init);
module_exit(sc2_exit);
MODULE_DESCRIPTION(
	"hardware 3D MVC through the legacy video layer (Fire TV Cube 3rd gen / T7 port of the S905X4 module)");
MODULE_LICENSE("GPL");
MODULE_VERSION(MVC3D_MODULE_VERSION);
