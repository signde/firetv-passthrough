/* Gazelle port candidate. NOT LOAD-TESTED. Requires open-driver FEL fixes. */
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dvfel (SC2) - Dolby Vision FEL composition by the Dolby Vision core of an
 * Amlogic S905X4 (SC2) on Android firmwares with the 5.4.125 kernel (Dune HD).
 *
 * The SoC composes BL + EL itself when the Dolby Vision driver gets the EL
 * picture of each frame from the "dveldec" provider (vfm: dveldec -> dvel), as
 * in the stream mode dual layer decoder. Android players decode with
 * MediaCodec (frame mode), which outputs the base layer only, so:
 *
 *   - the player reports every base layer access unit it queues (packet pts +
 *     RPU CRC, DVFEL_IOC_SC2_BL_HINT), decodes the EL with a second MediaCodec
 *     into an AImageReader and queues each picture here with the pts of its
 *     frame (DVFEL_IOC_SC2_EL_QUEUE: uvm dma-buf); dvfel registers as
 *     "dveldec" and passes the decoder's own vframe of that picture on;
 *   - kprobes on is_dovi_dual_layer_frame() and dolby_vision_parse_metadata()
 *     see each base layer frame the driver processes: its RPU CRC names its
 *     pts (the earliest pending hint with that CRC, as the S5 dvfel does in
 *     userspace); when the EL picture of that pts is queued, older ones are
 *     dropped, the frame is marked dual layer (src_fmt.dual_layer, which the
 *     driver reads in frame mode) and is_dovi_dual_layer_frame() returns true
 *     (it asks a "dvbldec" provider, which frame mode does not have); the
 *     driver then peeks/gets the head of the dveldec queue, that picture;
 *   - Dolby Vision FEL is switched on in the driver (debug "enable_fel 1").
 *
 * The decoder keeps an EL picture's buffer until the display is done with it:
 * each queued picture gets a release fence (sync file) that the player gives
 * to AImage_deleteAsync() at once; it is signalled when the driver puts the
 * picture back (or it is dropped), and the id is reported to the player
 * (DVFEL_IOC_SC2_WAIT_DONE) for its pacing. The reader's images are not held:
 * the decoder allows only 12 output buffers in all.
 *
 * Layout: built against 5.4.125 headers (CoreELEC 7c5e663^1); of struct
 * vframe_s only src_fmt is used, its inner layout as in the Dune aml_media
 * binary. Its offset differs (Dune r24: 1480, headers: 1528), so it is
 * measured at load with the firmware's own clear_vframe_src_fmt(); the base
 * layer frames and the EL vframes are the firmware's, never copied.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/dma-buf.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/dma-fence.h>
#include <linux/sync_file.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/input.h>
#include <asm/unaligned.h>
#include <linux/amlogic/media/vfm/vframe.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#include <linux/amlogic/meson_uvm_core.h>
#include <linux/amlogic/media/codec_mm/codec_mm.h>
#include <linux/amlogic/media/canvas/canvas.h>

#include "dvfel_sc2_uapi.h"

#define DRV_NAME	"dvfel"
#define EL_PROV_NAME	"dveldec"
#define DV_DEBUG_PATH	"/sys/class/amdolby_vision/debug"
#define AUX_DV_SEI	0x01000000	/* aux record of the Dolby Vision RPU NAL unit */
#define MAX_EL		DVFEL_SC2_MAX_EL
#define MAX_HINTS	DVFEL_SC2_MAX_HINTS
#define DONE_RING	64
/* pending hints a frame may skip (frames dropped before the display) */
#define HINT_SEARCH	MAX_HINTS
/* Bounded drain observations; elapsed time NEVER authorizes a buffer release. */
#define DRAIN_TRIES 100

/* src_fmt in the struct vframe_s of the Dune r24 SC2 aml_media (measured at load, see below) */
#define SC2_VF_SRC_FMT_OFF	1496 /* PS7717: verified against clear_vframe_src_fmt */

/* Keep this candidate inactive until the separate open-driver patch is verified. */
static bool open_driver_fixed;
module_param(open_driver_fixed, bool, 0444);
MODULE_PARM_DESC(open_driver_fixed, "Experimental: requires separately verified open-driver FEL fixes");

static unsigned int seek_wait_ms;
module_param(seek_wait_ms,uint,0644);
MODULE_PARM_DESC(seek_wait_ms,"Bounded EL wait after flush, 0 uses normal 50ms; max250ms");
static bool wait_el;
module_param(wait_el, bool, 0644);
MODULE_PARM_DESC(wait_el, "Bounded EL wait experiment; default off");

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "1: session events, 2: every EL picture");

#define dbg(lvl, fmt, ...) \
	do { if (debug >= (lvl)) pr_info(DRV_NAME ": " fmt, ##__VA_ARGS__); } while (0)

enum el_state { EL_FREE, EL_QUEUED, EL_OUT, EL_RELEASE };

struct el_ent {
	enum el_state state;
	struct dma_buf *db;
	struct vframe_s *vf;
	struct vframe_s *bl_vf;
	u64 bl_native;
	u32 bl_crc;
	bool display_pin;
	bool v4l_ref; /* hold VF_PROCESS_V4LVIDEO until the release fence */
	struct dma_fence *fence;	/* the decoder waits on it before reusing the buffer */
	u32 crc;
	u32 id;
	s64 pts_us;
	unsigned long out_jiffies;
};

struct bl_hint {
	s64 pts_us;
	u32 crc;
};

static struct dvfel_sc2 {
	spinlock_t lock;		/* everything below; taken in the vsync interrupt */
	struct el_ent ent[MAX_EL];
	struct el_ent *q[MAX_EL];	/* queued, in pts (display) order */
	int nq;
	struct bl_hint hint[MAX_HINTS];	/* base layer access units, in pts order */
	int nh;
	u64 fence_ctx;
	u32 fence_seq;
	spinlock_t fence_lock;
	u32 next_id;
	u32 done[DONE_RING];
	u32 done_head, done_tail;
	wait_queue_head_t done_wq;
	struct work_struct rel_work;
	struct delayed_work orphan_work;

	struct mutex sess_lock;		/* session start/stop */
	struct mutex ioctl_lock;		/* user commands versus stop/restart */
	u64 session_id;
	struct file *owner;
	bool active;			/* pairing on (kprobes act) */
	bool prov_reg;
	bool closing;
	unsigned int drain_checks;
	bool bl_detached;
	u64 el_retired;
	struct vframe_provider_s prov;

	struct vframe_s *bl_vf;		/* last base layer frame paired */
	bool bl_found;
 bool gap_pending;
 unsigned int gap_good_frames;
 unsigned long gap_deadline, gap_ready_after;
 u64 gap_epochs, gap_blank_checks;
	u64 bl_native;
	u32 bl_crc;
	u64 bl_reused;

	struct vframe_s *wait_vf;
	u32 wait_crc;
	u64 wait_native;
	unsigned long wait_until;
	bool wait_expired;
	u64 wait_checks, wait_timeouts, wait_ready;
 unsigned long seek_wait_until;
 u64 seek_wait_extended;

	/* stats */
	u64 el_in, el_out, el_put, el_dropped, el_flushed, bl_paired, bl_no_el, bl_no_crc, bl_no_hint;
	u64 hints_in, hints_skipped, hints_overflow;
} g;

#include "live_fixes.h"

/*
 * A/B: the EL off / on at once with the remote's Netflix button while a session runs, in any FEL
 * player (see the A/B section below). Every EL goes to the display as a module copy of the decoder's
 * vframe (ab_vf), so that the EL of a paused picture can be re-pointed.
 */
static struct vframe_s ab_vf[MAX_EL];
static bool ab_copies;	/* hand out copies: the firmware's vframe layout matches the headers */

static struct kretprobe krp_dual, krp_wait;
static struct kprobe kp_parse;
static bool krp_dual_ok, kp_parse_ok;
static struct dentry *dbg_dir;

/* offset of src_fmt in the firmware's struct vframe_s, measured at load; -1: unknown */
static int src_fmt_off = -1;

static inline struct vframe_src_fmt_s *sc2_src_fmt(struct vframe_s *vf)
{
	return (struct vframe_src_fmt_s *)((u8 *)vf + src_fmt_off);
}

/*
 * The firmware's own clear_vframe_src_fmt() writes src_fmt (fmt = -1, the
 * pointers and sizes 0, dual_layer 0) only where it finds the SEI magic code
 * (src_fmt + 4). With the magic in every word of a scratch buffer, the word
 * set to -1 is where its src_fmt is. Another layout than the Dune r24 build
 * (other firmwares) is used as measured; none found: no pairing (the frames
 * play as base layer + RPU) instead of writing into a wrong place.
 */
#define SEI_MAGIC	0x53656920	/* "Sei " */
static int measure_src_fmt_off(void)
{
	const int lo = 512, hi = 3072;
	int off = -1, k, n = 0;
	u8 *buf = kzalloc(hi + 256, GFP_KERNEL);

	if (!buf)
		return -1;
	for (k = lo; k < hi; k += 4)
		*(u32 *)(buf + k) = SEI_MAGIC;
	if (clear_vframe_src_fmt((struct vframe_s *)buf) == 0) {
		for (k = lo; k < hi - 24; k += 4) {
			if (*(u32 *)(buf + k) != 0xffffffff)
				continue;
			/* sei_ptr, sei_size cleared; dual_layer (a bool) cleared */
			if (*(u64 *)(buf + k + 8) || *(u32 *)(buf + k + 16) || buf[k + 20])
				continue;
			off = k;
			n++;
		}
	}
	kfree(buf);
	return n == 1 ? off : -1;
}

/* rpu_data_crc32 of the RPU attached to a base layer frame (as dvfel S5) */
static bool rpu_crc_of(struct vframe_s *vf, u32 *crc)
{
	struct vframe_src_fmt_s *sf = sc2_src_fmt(vf);
	const u8 *p = sf->sei_ptr, *end;
	u8 win[5] = { 0 }, last[5] = { 0 };
	u32 size, type, i, n = 0;
	bool have = false;
	int zeros = 0;

	if (!p || sf->sei_size < 8 || sf->sei_size > 64 * 1024)
		return false;
	end = p + sf->sei_size;
	while (end - p >= 8) {
		size = get_unaligned_be32(p);
		type = get_unaligned_be32(p + 4);
		p += 8;
		if (!size || size > end - p)
			return false;
		if (type != AUX_DV_SEI) {
			p += size;
			continue;
		}
		/* EBSP -> RBSP; the 5 bytes that end with the last non-zero one */
		for (i = 0; i < size; i++) {
			u8 b = p[i];

			if (zeros >= 2 && b == 3) {
				zeros = 0;
				continue;
			}
			zeros = b ? 0 : zeros + 1;
			memmove(win, win + 1, 4);
			win[4] = b;
			n++;
			if (b && n >= 5) {
				memcpy(last, win, 5);
				have = true;
			}
		}
		if (!have || last[4] != 0x80)
			return false;
		*crc = get_unaligned_be32(last);
		return true;
	}
	return false;
}

/* (g.lock held) remove q[i], keeping the order */
static struct el_ent *q_remove(int i)
{
	struct el_ent *e = g.q[i];

	memmove(&g.q[i], &g.q[i + 1], (g.nq - i - 1) * sizeof(e));
	g.nq--;
	return e;
}

/* PS7717 native v4lvideo buffers keep vf at offset zero of hook->arg.
 * The running v4lvideo_get_vf/init_file_private_data implementations are
 * guarded at load. Avoid relying on the full, differing header layout. */
static void el_vframe_put(struct dma_buf *db, bool v4l_ref)
{
 if (v4l_ref) uvm_put_hook_mod(db, VF_PROCESS_V4LVIDEO);
 else dmabuf_put_vframe(db);
}

static struct vframe_s *el_vframe_get(struct dma_buf *db, bool *v4l_ref)
{
 struct uvm_hook_mod *hook;
 struct vframe_s *vf;
 u32 flag;
 BUILD_BUG_ON(offsetof(struct uvm_hook_mod, arg) != 8);
 BUILD_BUG_ON(VF_PROCESS_V4LVIDEO != 2);
 *v4l_ref = false;
 vf = dmabuf_get_vframe(db);
 if (IS_ERR(vf) || vf) return vf;
 /* This API acquires a reference even when no decoder vf is attached. */
 dmabuf_put_vframe(db);
 hook = uvm_get_hook_mod(db, VF_PROCESS_V4LVIDEO);
 if (IS_ERR(hook)) return ERR_CAST(hook);
 if (!hook) return ERR_PTR(-ENOENT);
 if (!hook->arg || probe_kernel_read(&flag, (char *)hook->arg + 0xdd0, 4) || flag) {
  uvm_put_hook_mod(db, VF_PROCESS_V4LVIDEO);
  return ERR_PTR(-EINVAL); /* no deinterlaced/alternate frame path in this test */
 }
 *v4l_ref = true;
 return (struct vframe_s *)hook->arg;
}

/* (g.lock held) the entry goes back to the decoder: release in process context */
static void ent_release_locked(struct el_ent *e)
{
	e->state = EL_RELEASE;
	schedule_work(&g.rel_work);
}

/* PS7717's video_render unregistration drops BL references without returning
 * their EL partners. Retire only after that callback completes AND VD2 AFBC is
 * physically disabled. Use the driver's own unregistration exclusion while
 * detaching only our frames from its bookkeeping. Never dereference dead BLs. */
static bool retire_stopped_frames(void)
{
 atomic_t *unreg=(void *)0xffffffc011b5fc30UL;
 atomic_t *inirq=(void *)0xffffffc011b5fc3cUL;
 spinlock_t *dvlock=(void *)0xffffffc011b99aecUL;
 struct vframe_s **cur=(void *)0xffffffc011b5fc80UL;
 unsigned long flags;int tries,i,id,j,retired=0;
 if(!READ_ONCE(g.bl_detached) || READ_ONCE(g.active) ||
    read_dv_reg(0x48c0)!=0 || ((read_dv_reg(0x1a71)>>4)&3)!=3)
  return false;
 atomic_inc(unreg);smp_mb();
 for(tries=0;atomic_read(inirq)>0 && tries<100;tries++)msleep(1);
 if(atomic_read(inirq)>0 || read_dv_reg(0x48c0)!=0){atomic_dec(unreg);return false;}
 spin_lock_irqsave(dvlock,flags);
 spin_lock(&g.lock);
 for(i=0;i<MAX_EL;i++){
  struct el_ent *e=&g.ent[i];
  if(e->state!=EL_OUT)continue;
  /* Layout independently checked in amdv_vf_put's guarded live code. */
  for(id=0;id<2;id++)for(j=0;j<16;j++){
   struct vframe_s **pair=(void *)(0xffffffc011ba4048UL+id*0x188+0x58+j*16);
   if(READ_ONCE(pair[1])==e->vf||READ_ONCE(pair[1])==&ab_vf[i]){WRITE_ONCE(pair[1],NULL);WRITE_ONCE(pair[0],NULL);}
  }
  if(READ_ONCE(*cur)==e->vf||READ_ONCE(*cur)==&ab_vf[i])WRITE_ONCE(*cur,NULL);
  ent_release_locked(e);retired++;g.el_retired++;
 }
 spin_unlock(&g.lock);
 spin_unlock_irqrestore(dvlock,flags);
 smp_mb();atomic_dec(unreg);
 if(retired)pr_info("dvfel: retired %d EL frames after BL detach and hardware off\n",retired);
 return true;
}

static void rel_work_fn(struct work_struct *work)
{
	struct el_ent *rel[MAX_EL];
	unsigned long flags;
	int i, n = 0;

	spin_lock_irqsave(&g.lock, flags);
	for (i = 0; i < MAX_EL; i++) {
		struct el_ent *e = &g.ent[i];

		if (e->state != EL_RELEASE)
			continue;
		rel[n++] = e;
	}
	spin_unlock_irqrestore(&g.lock, flags);

	for (i = 0; i < n; i++) {
		struct el_ent *e = rel[i];

		el_vframe_put(e->db, e->v4l_ref);
		dma_buf_put(e->db);
		/* the decoder may decode into the buffer again */
		if (e->fence) {
			dma_fence_signal(e->fence);
			dma_fence_put(e->fence);
			e->fence = NULL;
		}
		spin_lock_irqsave(&g.lock, flags);
		if (g.done_head - g.done_tail < DONE_RING)
			g.done[g.done_head++ % DONE_RING] = e->id;
		e->db = NULL;
		e->vf = NULL;
		e->state = EL_FREE;
		if(e->display_pin){e->display_pin=false;module_put(THIS_MODULE);}
		spin_unlock_irqrestore(&g.lock, flags);
	}
	if (n)
		wake_up_interruptible(&g.done_wq);
}

/* (g.lock held) the pts of a frame by its RPU CRC: the earliest pending hint */
static bool pts_of_crc(u32 crc, s64 *pts)
{
	int i, n = min(g.nh, HINT_SEARCH);

	for (i = 0; i < n; i++) {
		if (g.hint[i].crc != crc)
			continue;
		*pts = g.hint[i].pts_us;
		g.hints_skipped += i;
		memmove(&g.hint[0], &g.hint[i + 1], (g.nh - i - 1) * sizeof(g.hint[0]));
		g.nh -= i + 1;
		return true;
	}
	return false;
}

/*
 * A base layer frame the Dolby Vision driver processes (vsync interrupt):
 * find its EL picture and put it at the head of the queue.
 */
/* Keep a pairing for as long as the display owns its EL, including pause
 * and metadata reprocessing of an older displayed BL. No freed VF dereference. */
static bool paired_owned_locked(struct vframe_s *vf, u64 native, u32 crc)
{
 int i;
 for (i=0; i<MAX_EL; i++) {
  struct el_ent *e=&g.ent[i];
  if ((e->state==EL_QUEUED || e->state==EL_OUT) && e->bl_vf==vf &&
      e->bl_native==native && e->bl_crc==crc) return true;
 }
 return false;
}
static bool pair_bl(struct vframe_s *vf)
{
	unsigned long flags;
	bool found = false;
	s64 pts;
	u32 crc;
	u64 native;

	if (!vf || !READ_ONCE(g.active) || src_fmt_off < 0)
		return false;
	if (!rpu_crc_of(vf, &crc)) {
		spin_lock_irqsave(&g.lock, flags);
		g.bl_no_crc++;
		spin_unlock_irqrestore(&g.lock, flags);
		return false;
	}
	native=get_unaligned((u64 *)((u8 *)vf + 0x28));
 spin_lock_irqsave(&g.lock, flags);
 if (paired_owned_locked(vf,native,crc)) {
  found=true;
  g.bl_reused++;
 } else if (g.bl_vf == vf && g.bl_native==native && g.bl_crc==crc) {
		/* asked again for the same frame (wait, then parse) */
		found = false; /* its EL is no longer owned */
	} else if (!pts_of_crc(crc, &pts)) {
		/* decoded before a seek, or not reported */
		g.bl_no_hint++;
		if (g.bl_no_hint <= 12 || !(g.bl_no_hint % 120)) {
			int match = -1, j;
			for (j = 0; j < g.nh; j++)
				if (g.hint[j].crc == crc) { match = j; break; }
			pr_info("dvfel: no hint crc=%08x pending=%d fullmatch=%d head=%08x elq=%d firstpts=%lld\n",
				crc, g.nh, match, g.nh ? g.hint[0].crc : 0,
				g.nq, g.nq ? g.q[0]->pts_us : -1LL);
		}
		g.bl_vf = vf;
		g.bl_found = false;
	} else {
		/* pts_us64 offset verified against stock amdv_wait_metadata_v1. */
		if (g.bl_paired + g.bl_no_el < 12 || !((g.bl_paired + g.bl_no_el) % 60))
			pr_info("dvfel: timestamps native=%llu hint=%lld crc=%08x elfirst=%lld ellast=%lld\n",
				get_unaligned((u64 *)((u8 *)vf + 0x28)), pts, crc,
				g.nq ? g.q[0]->pts_us : -1LL,
				g.nq ? g.q[g.nq-1]->pts_us : -1LL);
		/* older ones: their frames are gone */
		while (g.nq && g.q[0]->pts_us < pts) {
			struct el_ent *e = q_remove(0);

			g.el_dropped++;
			dbg(2, "EL id %u pts %lld dropped (frame %lld)\n", e->id, e->pts_us, pts);
			ent_release_locked(e);
		}
		found = g.nq && g.q[0]->pts_us == pts;
		if (found) {
   g.q[0]->bl_vf=vf;
   g.q[0]->bl_native=native;
   g.q[0]->bl_crc=crc;
   g.bl_paired++;
   if (g.gap_pending) {
    g.gap_good_frames++;
    if (g.gap_good_frames == 2)
     g.gap_ready_after=jiffies+msecs_to_jiffies(34);
    else if (g.gap_good_frames > 2 && time_after_eq(jiffies,g.gap_ready_after))
     g.gap_pending=false;
   }
  }
		else
			g.bl_no_el++;
		g.bl_vf = vf;
		g.bl_found = found;
	}
	if (!found && blank_seek_gaps) {
  gap_arm_locked(false);
  g.gap_good_frames=0;
  g.gap_ready_after=g.gap_deadline;
 }
 g.bl_vf=vf; g.bl_native=native; g.bl_crc=crc; g.bl_found=found;
 if (found)
  sc2_src_fmt(vf)->dual_layer = true;
	spin_unlock_irqrestore(&g.lock, flags);
	return found;
}

/* The T7 multi-DV wait path omits EL synchronization. Preserve every stock
 * nonzero return and only hold our VD1 frame briefly while its known EL arrives.
 * Never sleep in vsync and never release a frame already handed to the display.
 */
static int wait_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	*(struct vframe_s **)ri->data = regs->regs[1] == 0 ?
		(struct vframe_s *)regs->regs[0] : NULL;
	return 0;
}

static int wait_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct vframe_s *vf = *(struct vframe_s **)ri->data;
	unsigned long flags;
	u32 crc;
	s64 pts = 0;
	int i;
	bool have = false;

	if (!READ_ONCE(wait_el) || regs->regs[0] || !vf || !READ_ONCE(g.active) || !rpu_crc_of(vf, &crc))
		return 0;
	spin_lock_irqsave(&g.lock, flags);
	if (!g.active || (g.bl_vf == vf && g.bl_native == get_unaligned((u64 *)((u8 *)vf + 0x28)) && g.bl_crc == crc))
		goto out;
	for (i = 0; i < min(g.nh, HINT_SEARCH); i++) {
		if (g.hint[i].crc == crc) {
			pts = g.hint[i].pts_us;
			have = true;
			break;
		}
	}
	if (!have)
		goto out;
	if (g.wait_vf != vf || g.wait_crc != crc || g.wait_native != get_unaligned((u64 *)((u8 *)vf + 0x28))) {
  unsigned int timeout=50;
		g.wait_vf = vf;
		g.wait_native=get_unaligned((u64 *)((u8 *)vf + 0x28));
		g.wait_crc = crc;
  if (seek_wait_ms && time_before(jiffies,g.seek_wait_until)) {
   timeout=clamp_t(unsigned int,seek_wait_ms,50,250);
   g.seek_wait_extended++;
  }
		g.wait_until = jiffies + msecs_to_jiffies(timeout);
		g.wait_expired = false;
	}
	while (g.nq && g.q[0]->pts_us < pts) {
		g.el_dropped++;
		ent_release_locked(q_remove(0));
	}
	if (g.nq && g.q[0]->pts_us >= pts) {
		if (g.q[0]->pts_us == pts)
			g.wait_ready++;
		goto out; /* newer EL proves this picture has already been skipped */
	}
	if (time_before(jiffies, g.wait_until)) {
		g.wait_checks++;
		regs->regs[0] = 1;
	} else if (!g.wait_expired) {
		g.wait_expired = true;
		g.wait_timeouts++;
  if (g.wait_timeouts <= 16) {
   int held=0,j;
   for(j=0;j<MAX_EL;j++) held+=g.ent[j].state==EL_OUT;
   pr_info("dvfel: EL wait expired pts=%lld queued=%d displayed=%d hints=%d\n",pts,g.nq,held,g.nh);
  }
	}
out:
	spin_unlock_irqrestore(&g.lock, flags);
	return 0;
}

/* is_dovi_dual_layer_frame(vf): true for a frame paired here */
static int dual_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	*(bool *)ri->data = pair_bl((struct vframe_s *)regs->regs[0]);
	return 0;
}

static int dual_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	if (*(bool *)ri->data)
		regs->regs[0] = 1;
	return 0;
}

/* dolby_vision_parse_metadata(vf, ...): marks the frame before the EL get */
static int parse_pre(struct kprobe *p, struct pt_regs *regs)
{
	pair_bl((struct vframe_s *)regs->regs[0]);
	return 0;
}

/* --- A/B: the EL switched off without the player ---
 *
 * The remote's Netflix button (ab_key) no longer reaches Android while a session runs; each press
 * switches el_mute. With the EL off the frames stay paired, but the display gets a neutral EL picture
 * (no change to the base layer) from a module buffer instead of the decoder's, so VD2 and the composer
 * keep their FEL setup and the picture is not blanked. The neutral picture is uncompressed 10-bit 4:2:0
 * as the VD2 input reads it: the luma in 3 bytes per pixel (the value at the top of the first 12-bit
 * field), the chroma in 12-bit Cr/Cb pairs (found with the VPP pixel probe against 8-bit NV21). A
 * change of the EL's kind is announced to the video driver by a width 32 pixels narrower for one frame,
 * so it reprograms VD2. On a switch the EL on screen is re-pointed at once; on a paused picture the
 * video driver applies it when asked to set the shown frames up again (force_property_change), which
 * video/fel-ab-notify.sh does (SELinux refuses that write from a kernel thread); it also shows the state.
 */
#define AB_W		1920
#define AB_H		1080
#define AB_YSTRIDE	(AB_W * 3)
#define AB_CSTRIDE	(AB_W * 3 / 2)
#define AB_SIZE		PAGE_ALIGN(AB_YSTRIDE * AB_H + AB_CSTRIDE * AB_H / 2)

static unsigned int ab_key = 0x2e8;
module_param(ab_key, uint, 0644);
MODULE_PARM_DESC(ab_key, "Key code of the A/B (EL off/on) button while a session runs (Fire TV remote Netflix: 0x2e8); 0 = none");
static bool el_mute;
static unsigned long ab_muted, ab_presses;
static unsigned long mute_phys;
static u8 *mute_virt;
static int ab_seen = -1;	/* the kind of EL last handed out: 0 the decoder's, 1 neutral */
static int ab_shown = -1;	/* entry of the EL last handed to the display */

/* the neutral picture: 512 in every component; two 12-bit fields per 3 bytes, the value << 2 */
static void ab_fill_line(u8 *d, int fields)
{
	const u32 v = 512 << 2, w = v | v << 12;
	int k;

	for (k = 0; k < fields / 2; k++, d += 3) {
		d[0] = w & 0xff;
		d[1] = (w >> 8) & 0xff;
		d[2] = (w >> 16) & 0xff;
	}
}

static int mute_alloc(void)
{
	int y;

	if (mute_phys)
		return 0;
	mute_phys = codec_mm_alloc_for_dma(DRV_NAME "-mute", AB_SIZE / PAGE_SIZE, 0, CODEC_MM_FLAGS_DMA);
	if (!mute_phys)
		return -ENOMEM;
	mute_virt = codec_mm_vmap(mute_phys, AB_SIZE);
	if (!mute_virt) {
		codec_mm_free_for_dma(DRV_NAME "-mute", mute_phys);
		mute_phys = 0;
		return -ENOMEM;
	}
	for (y = 0; y < AB_H; y++)
		ab_fill_line(mute_virt + y * AB_YSTRIDE, AB_W * 2);	/* 24 bits per luma pixel */
	for (y = 0; y < AB_H / 2; y++)
		ab_fill_line(mute_virt + AB_YSTRIDE * AB_H + y * AB_CSTRIDE, AB_W);
	codec_mm_dma_flush(mute_virt, AB_SIZE, DMA_TO_DEVICE);
	return 0;
}

static void mute_free(void)
{
	if (!mute_phys)
		return;
	codec_mm_unmap_phyaddr(mute_virt);
	codec_mm_free_for_dma(DRV_NAME "-mute", mute_phys);
	mute_phys = 0;
	mute_virt = NULL;
}

/* g.lock held: the vframe to hand out for entry e (the decoder's, or a copy: neutral when muted) */
static struct vframe_s *el_vf_out(struct el_ent *e, bool get)
{
	int i = e - g.ent;
	struct vframe_s *t = &ab_vf[i];
	bool mute = READ_ONCE(el_mute) && mute_phys;
	int seen = mute ? 1 : 0;
	bool narrow = seen != ab_seen;

	if (!READ_ONCE(ab_copies))
		return e->vf;
	memcpy(t, e->vf, sizeof(*t));
	if (mute) {
		t->type &= ~(VIDTYPE_COMPRESS | VIDTYPE_SCATTER | VIDTYPE_SUPPORT_COMPRESS | VIDTYPE_VIU_422 |
			     VIDTYPE_VIU_SINGLE_PLANE | VIDTYPE_VIU_444 | VIDTYPE_VIU_NV12);
		t->type |= VIDTYPE_VIU_NV21 | VIDTYPE_VIU_FIELD | VIDTYPE_NO_DW;
		t->bitdepth = BITDEPTH_Y10 | BITDEPTH_U10 | BITDEPTH_V10;
		t->flag |= VFRAME_FLAG_VIDEO_LINEAR;
		t->width = AB_W;
		t->height = AB_H;
		t->canvas0Addr = (u32)-1;
		t->canvas1Addr = (u32)-1;
		t->plane_num = 2;
		memset(&t->canvas0_config, 0, sizeof(t->canvas0_config));
		t->canvas0_config[0].phy_addr = mute_phys;
		t->canvas0_config[0].width = AB_YSTRIDE;
		t->canvas0_config[0].height = AB_H;
		t->canvas0_config[0].block_mode = CANVAS_BLKMODE_LINEAR;
		t->canvas0_config[1].phy_addr = mute_phys + AB_YSTRIDE * AB_H;
		t->canvas0_config[1].width = AB_CSTRIDE;
		t->canvas0_config[1].height = AB_H / 2;
		t->canvas0_config[1].block_mode = CANVAS_BLKMODE_LINEAR;
		memcpy(t->canvas1_config, t->canvas0_config, sizeof(t->canvas1_config));
	}
	if (narrow) {
		t->width -= 32;
		t->compWidth -= 32;
	}
	if (get) {
		ab_seen = seen;
		if (mute)
			ab_muted++;
	}
	return t;
}

/* after a switch (process context): the neutral picture ready, the EL on screen re-pointed */
static void ab_work_fn(struct work_struct *w)
{
	unsigned long flags;

	if (READ_ONCE(el_mute) && mute_alloc()) {
		pr_warn(DRV_NAME ": no memory for the neutral EL picture: EL off unavailable\n");
		WRITE_ONCE(el_mute, false);
		return;
	}
	spin_lock_irqsave(&g.lock, flags);
	if (g.active && ab_shown >= 0 && g.ent[ab_shown].state == EL_OUT)
		el_vf_out(&g.ent[ab_shown], false);
	spin_unlock_irqrestore(&g.lock, flags);
}
static DECLARE_WORK(ab_work, ab_work_fn);

static int el_mute_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (!ret)
		schedule_work(&ab_work);
	return ret;
}
static const struct kernel_param_ops el_mute_ops = { .set = el_mute_set, .get = param_get_bool };
module_param_cb(el_mute, &el_mute_ops, &el_mute, 0644);
MODULE_PARM_DESC(el_mute, "EL off: a neutral EL picture instead of the decoder's (the A/B button switches it)");

static bool ab_filter(struct input_handle *h, unsigned int type, unsigned int code, int value)
{
	if (type != EV_KEY || !code || code != READ_ONCE(ab_key) || !READ_ONCE(g.owner))
		return false;
	if (value == 1) {
		WRITE_ONCE(el_mute, !READ_ONCE(el_mute));
		ab_presses++;
		schedule_work(&ab_work);
		pr_info(DRV_NAME ": A/B button: EL %s\n", el_mute ? "off" : "on");
	}
	return true; /* not to Android while a session runs */
}

static int ab_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *h = kzalloc(sizeof(*h), GFP_KERNEL);
	int ret;

	if (!h)
		return -ENOMEM;
	h->dev = dev;
	h->handler = handler;
	h->name = DRV_NAME "_ab";
	ret = input_register_handle(h);
	if (ret)
		goto err_free;
	ret = input_open_device(h);
	if (ret)
		goto err_unreg;
	return 0;
err_unreg:
	input_unregister_handle(h);
err_free:
	kfree(h);
	return ret;
}

static void ab_disconnect(struct input_handle *h)
{
	input_close_device(h);
	input_unregister_handle(h);
	kfree(h);
}

static bool ab_match(struct input_handler *handler, struct input_dev *dev)
{
	return ab_key && ab_key <= KEY_MAX && test_bit(ab_key, dev->keybit);
}

static const struct input_device_id ab_ids[] = {
	{ .flags = INPUT_DEVICE_ID_MATCH_EVBIT, .evbit = { BIT_MASK(EV_KEY) } },
	{ },
};

static struct input_handler ab_handler = {
	.filter = ab_filter,
	.match = ab_match,
	.connect = ab_connect,
	.disconnect = ab_disconnect,
	.name = DRV_NAME "_ab",
	.id_table = ab_ids,
};
static bool ab_handler_reg;

/* --- the "dveldec" provider, called by the Dolby Vision driver --- */

static struct vframe_s *prov_peek(void *op_arg)
{
	struct vframe_s *vf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&g.lock, flags);
	if (g.active && g.nq)
		vf = el_vf_out(g.q[0], false);
	spin_unlock_irqrestore(&g.lock, flags);
	return vf;
}

static struct vframe_s *prov_get(void *op_arg)
{
	struct vframe_s *vf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&g.lock, flags);
	if (g.active && g.nq) {
		struct el_ent *e = q_remove(0);

		e->state = EL_OUT;
		e->out_jiffies = jiffies;
		__module_get(THIS_MODULE); /* retain code while a display frame is outstanding */
		e->display_pin = true;
		vf = el_vf_out(e, true);
		ab_shown = e - g.ent;
		g.el_out++;
		dbg(2, "EL id %u crc %08x to display\n", e->id, e->crc);
	}
	spin_unlock_irqrestore(&g.lock, flags);
	return vf;
}

static void prov_put(struct vframe_s *vf, void *op_arg)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&g.lock, flags);
	for (i = 0; i < MAX_EL; i++) {
		struct el_ent *e = &g.ent[i];

		if (e->state == EL_OUT && (e->vf == vf || &ab_vf[i] == vf)) {
			g.el_put++;
			ent_release_locked(e);
			break;
		}
	}
	spin_unlock_irqrestore(&g.lock, flags);
	if (i == MAX_EL)
		dbg(1, "put of an unknown EL frame %p\n", vf);
	if (READ_ONCE(g.closing))
		schedule_delayed_work(&g.orphan_work,msecs_to_jiffies(100));
}

static int prov_event(int type, void *data, void *op_arg)
{
	return 0;
}

static int prov_states(struct vframe_states *states, void *op_arg)
{
	unsigned long flags;

	spin_lock_irqsave(&g.lock, flags);
	states->vf_pool_size = MAX_EL;
	states->buf_recycle_num = 0;
	states->buf_free_num = MAX_EL - g.nq;
	states->buf_avail_num = g.nq;
	spin_unlock_irqrestore(&g.lock, flags);
	return 0;
}

static const struct vframe_operations_s prov_ops = {
	.peek = prov_peek,
	.get = prov_get,
	.put = prov_put,
	.event_cb = prov_event,
	.vf_states = prov_states,
};

/* --- session --- */

/* drop the queued EL pictures (not the ones on display) and / or the hints */
static void flush_queue(u32 what)
{
	unsigned long flags;

	spin_lock_irqsave(&g.lock, flags);
	if (what & DVFEL_SC2_FLUSH_EL) {
  if (g.active) g.seek_wait_until=jiffies+msecs_to_jiffies(1500);
  if (g.active && blank_seek_gaps) gap_arm_locked(true);
  else if (!g.active) g.gap_pending=false;
		while (g.nq) {
			g.el_flushed++;
			ent_release_locked(q_remove(0));
		}
		g.wait_vf = NULL;
		g.bl_vf = NULL;
		g.bl_found = false;
	}
	if (what & DVFEL_SC2_FLUSH_HINTS)
		g.nh = 0;
	spin_unlock_irqrestore(&g.lock, flags);
}

static long ioc_bl_hint(struct file *f, void __user *arg)
{
	struct dvfel_sc2_hint r;
	unsigned long flags;
	int i;

	if (copy_from_user(&r, arg, sizeof(r)))
		return -EFAULT;
	if (g.owner != f)
		return -EPERM;
	if (r.reserved)
		return -EINVAL;
	spin_lock_irqsave(&g.lock, flags);
	g.hints_in++;
	if (g.nh == MAX_HINTS) {
		/* frames that never reached the display: the oldest goes */
		memmove(&g.hint[0], &g.hint[1], (MAX_HINTS - 1) * sizeof(g.hint[0]));
		g.nh--;
		g.hints_overflow++;
	}
	/* decode order in, display (pts) order kept */
	for (i = g.nh; i > 0 && g.hint[i - 1].pts_us > r.pts_us; i--)
		;
	memmove(&g.hint[i + 1], &g.hint[i], (g.nh - i) * sizeof(g.hint[0]));
	g.hint[i].pts_us = r.pts_us;
	g.hint[i].crc = r.rpu_crc;
	g.nh++;
	spin_unlock_irqrestore(&g.lock, flags);
	return 0;
}

static const char *fence_driver_name(struct dma_fence *f)
{
	return DRV_NAME;
}

static const char *fence_timeline_name(struct dma_fence *f)
{
	return "el";
}

static void el_fence_release(struct dma_fence *f)
{ kfree(f); module_put(THIS_MODULE); }

static const struct dma_fence_ops el_fence_ops = {
	.release = el_fence_release,
	.get_driver_name = fence_driver_name,
	.get_timeline_name = fence_timeline_name,
};

/* Keep the provider registered until actual display puts return all buffers.
 * Unregistering first makes dvel_vf_put unable to find us. A session pin keeps
 * the worker and callbacks alive. Never free a frame just because time passed. */
static void orphan_work_fn(struct work_struct *work)
{
 unsigned long flags;int i,n=0;
 mutex_lock(&g.sess_lock);
 if(!g.closing){mutex_unlock(&g.sess_lock);return;}
 spin_lock_irqsave(&g.lock,flags);
 for(i=0;i<MAX_EL;i++)n+=g.ent[i].state==EL_OUT;
 spin_unlock_irqrestore(&g.lock,flags);
 if(n && retire_stopped_frames()) n=0;
 if(n){
  if(++g.drain_checks<DRAIN_TRIES)
   schedule_delayed_work(&g.orphan_work,msecs_to_jiffies(100));
  else pr_warn("dvfel: drain waiting for %d real display puts; frames retained\n",n);
  mutex_unlock(&g.sess_lock);return;
 }
 if(g.prov_reg){vf_unreg_provider(&g.prov);g.prov_reg=false;}
 flush_work(&g.rel_work);
 WRITE_ONCE(g.closing,false);
 pr_info("dvfel: drain complete, provider detached\n");
 mutex_unlock(&g.sess_lock);
 module_put(THIS_MODULE);
}

static long sess_start(struct file *f)
{
	long ret = 0;

	mutex_lock(&g.sess_lock);
	if (g.closing || (g.owner && g.owner != f)) {
		ret = -EBUSY;
		goto out;
	}
	if (!g.prov_reg) {
		if (vf_get_provider(EL_PROV_NAME)) {
			/* a stream mode dual layer decoder is running */
			ret = -EBUSY;
			goto out;
		}
		WRITE_ONCE(g.bl_detached,false);
		__module_get(THIS_MODULE); /* session/drain lifetime */
		fixes_start();
		vf_provider_init(&g.prov, EL_PROV_NAME, &prov_ops, NULL);
		ret = vf_reg_provider(&g.prov);
		if (ret < 0){ fixes_stop(); module_put(THIS_MODULE); goto out; }
		ret = 0;
		g.prov_reg = true;
	}
	if (!g.owner) {
		unsigned long flags;
		spin_lock_irqsave(&g.lock, flags);
		g.done_head = g.done_tail = 0;
		spin_unlock_irqrestore(&g.lock, flags);
		WRITE_ONCE(g.session_id, g.session_id + 1);
		/* a new session starts with the EL */
		WRITE_ONCE(el_mute, false);
		ab_shown = -1;
	}
	WRITE_ONCE(g.owner, f);
	WRITE_ONCE(g.active, true);
	dbg(1, "session start\n");
out:
	mutex_unlock(&g.sess_lock);
	return ret;
}

static void sess_stop(struct file *f)
{
 mutex_lock(&g.sess_lock);
 if(g.owner!=f){mutex_unlock(&g.sess_lock);return;}
 WRITE_ONCE(g.active,false);
 WRITE_ONCE(g.closing,true);
 g.drain_checks=0;
 flush_queue(DVFEL_SC2_FLUSH_EL|DVFEL_SC2_FLUSH_HINTS);
 WRITE_ONCE(g.owner,NULL);
 wake_up_interruptible(&g.done_wq);
 fixes_stop();
 schedule_delayed_work(&g.orphan_work,msecs_to_jiffies(100));
 dbg(1,"session stop, waiting for display returns\n");
 mutex_unlock(&g.sess_lock);
}

static long ioc_el_queue(struct file *f, void __user *arg)
{
	struct dvfel_sc2_el r;
	struct el_ent *e = NULL;
	struct dma_buf *db;
	struct vframe_s *vf;
	struct dma_fence *fence;
	struct sync_file *sf = NULL;
	unsigned long flags;
	int i, fd;
	bool v4l_ref;

	if (copy_from_user(&r, arg, sizeof(r)))
		return -EFAULT;
	if (g.owner != f || !READ_ONCE(g.active))
		return -EPERM;
	if (r.reserved)
		return -EINVAL;
	db = dma_buf_get(r.fd);
	if (IS_ERR(db))
		return PTR_ERR(db);
	if (!dmabuf_is_uvm(db)) {
		dma_buf_put(db);
		return -ENODEV;
	}
	vf = el_vframe_get(db, &v4l_ref);
 if (IS_ERR_OR_NULL(vf)) { dma_buf_put(db); return vf ? PTR_ERR(vf) : -EINVAL; }
 /* A decoder buffer cannot occupy two live queue entries. */
 spin_lock_irqsave(&g.lock, flags);
 for (i = 0; i < MAX_EL; i++)
  if (g.ent[i].state != EL_FREE && g.ent[i].db == db) break;
 spin_unlock_irqrestore(&g.lock, flags);
 if (i != MAX_EL) {
  el_vframe_put(db, v4l_ref); dma_buf_put(db); return -EBUSY;
 }
 if (!g.el_in) pr_info("dvfel: native EL v4l=%d type=%x %ux%u depth=%x comp=%lx\n",
  v4l_ref, vf->type, vf->compWidth, vf->compHeight, vf->bitdepth, vf->compHeadAddr);

	/* the release fence: one reference for the sync file, one kept here */
	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fence && fd >= 0) {
		__module_get(THIS_MODULE); /* fence operations may outlive its queue entry */
		dma_fence_init(fence, &el_fence_ops, &g.fence_lock, g.fence_ctx, ++g.fence_seq);
		sf = sync_file_create(fence);
	}
	if (!sf) {
		if (fd >= 0)
			put_unused_fd(fd);
		if (fence && fence->ops)
			dma_fence_put(fence);
		else
			kfree(fence);
		el_vframe_put(db, v4l_ref);
		dma_buf_put(db);
		return -ENOMEM;
	}

	spin_lock_irqsave(&g.lock, flags);
	for (i = 0; i < MAX_EL; i++) {
		if (g.ent[i].state == EL_FREE) {
			e = &g.ent[i];
			break;
		}
	}
	if (e) {
		e->state = EL_QUEUED;
		e->bl_vf=NULL; e->bl_native=0; e->bl_crc=0;
		e->db = db;
		e->vf = vf;
		e->v4l_ref = v4l_ref;
		e->fence = fence;
		e->crc = r.rpu_crc;
		e->pts_us = r.pts_us;
		e->id = ++g.next_id;
		/* in pts order (the EL decoder's output is in display order) */
		for (i = g.nq; i > 0 && g.q[i - 1]->pts_us > r.pts_us; i--)
			;
		memmove(&g.q[i + 1], &g.q[i], (g.nq - i) * sizeof(e));
		g.q[i] = e;
		g.nq++;
		g.el_in++;
		r.id = e->id;
		r.queued = g.nq;
	}
	spin_unlock_irqrestore(&g.lock, flags);
	if (!e) {
		fput(sf->file);
		put_unused_fd(fd);
		dma_fence_signal(fence);
		dma_fence_put(fence);
		el_vframe_put(db, v4l_ref);
		dma_buf_put(db);
		return -EBUSY;
	}
	r.fence_fd = fd;
	dbg(2, "EL id %u crc %08x pts %lld queued (%u)\n", r.id, r.rpu_crc, r.pts_us, r.queued);
	if (copy_to_user(arg, &r, sizeof(r))) {
		/* the entry stays (released with the session); the fence with it */
		fput(sf->file);
		put_unused_fd(fd);
		return -EFAULT;
	}
	fd_install(fd, sf->file);
	return 0;
}

static bool done_ready(void)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&g.lock, flags);
	ready = g.done_head != g.done_tail;
	spin_unlock_irqrestore(&g.lock, flags);
	return ready;
}

static long ioc_wait_done(struct file *f, void __user *arg)
{
 struct dvfel_sc2_done r;
 unsigned long flags;
 u64 session;
 long ret;

 if (copy_from_user(&r, arg, sizeof(r))) return -EFAULT;
 if (r.timeout_ms > DVFEL_SC2_MAX_WAIT_MS) return -EINVAL;
 if (mutex_lock_interruptible(&g.ioctl_lock)) return -ERESTARTSYS;
 if (g.owner != f || !g.active) {
  mutex_unlock(&g.ioctl_lock); return -EPERM;
 }
 session = g.session_id;
 mutex_unlock(&g.ioctl_lock);
 if (r.timeout_ms) {
  ret = wait_event_interruptible_timeout(g.done_wq,
   done_ready() || READ_ONCE(g.owner) != f || !READ_ONCE(g.active) ||
   READ_ONCE(g.session_id) != session, msecs_to_jiffies(r.timeout_ms));
  if (ret < 0) return ret;
 }
 if (mutex_lock_interruptible(&g.ioctl_lock)) return -ERESTARTSYS;
 if (g.owner != f || !g.active || g.session_id != session) {
  ret = -EPERM; goto out;
 }
 r.count = 0;
 memset(r.id, 0, sizeof(r.id));
 spin_lock_irqsave(&g.lock, flags);
 while (g.done_head != g.done_tail && r.count < DVFEL_SC2_MAX_DONE)
  r.id[r.count++] = g.done[g.done_tail++ % DONE_RING];
 spin_unlock_irqrestore(&g.lock, flags);
 ret = copy_to_user(arg, &r, sizeof(r)) ? -EFAULT : 0;
out:
 mutex_unlock(&g.ioctl_lock);
 return ret;
}

static long dvfel_ioctl_locked(struct file *f, unsigned int cmd, unsigned long arg)
{
	void __user *p = (void __user *)arg;

	switch (cmd) {
	case DVFEL_IOC_SC2_INFO: {
		struct dvfel_sc2_info r = {};

		strscpy(r.version, DVFEL_SC2_VERSION, sizeof(r.version));
		return copy_to_user(p, &r, sizeof(r)) ? -EFAULT : 0;
	}
	case DVFEL_IOC_SC2_START:
		return sess_start(f);
	case DVFEL_IOC_SC2_EL_QUEUE:
		return ioc_el_queue(f, p);
	case DVFEL_IOC_SC2_FLUSH: {
		u32 what;

		if (g.owner != f)
			return -EPERM;
		if (get_user(what, (u32 __user *)p))
			return -EFAULT;
		if (what & ~(DVFEL_SC2_FLUSH_EL | DVFEL_SC2_FLUSH_HINTS))
			return -EINVAL;
		flush_queue(what);
		return 0;
	}
	case DVFEL_IOC_SC2_STOP:
		if (g.owner != f) return -EPERM;
		sess_stop(f);
		return 0;
	case DVFEL_IOC_SC2_BL_HINT:
		return ioc_bl_hint(f, p);
	default:
		return -ENOTTY;
	}
}

static long dvfel_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
 long ret;
 /* Completion waits must not block the producer or session teardown. */
 if (cmd == DVFEL_IOC_SC2_WAIT_DONE)
  return ioc_wait_done(f, (void __user *)arg);
 if (mutex_lock_interruptible(&g.ioctl_lock)) return -ERESTARTSYS;
 ret = dvfel_ioctl_locked(f, cmd, arg);
 mutex_unlock(&g.ioctl_lock);
 return ret;
}

static int dvfel_release(struct inode *inode, struct file *f)
{
	mutex_lock(&g.ioctl_lock);
	sess_stop(f);
	mutex_unlock(&g.ioctl_lock);
	return 0;
}

static const struct file_operations dvfel_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = dvfel_ioctl,
	.compat_ioctl = dvfel_ioctl,
	.release = dvfel_release,
};

static struct miscdevice dvfel_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DRV_NAME,
	.fops = &dvfel_fops,
	.mode = 0600,
};

static int stats_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i, out = 0;

	spin_lock_irqsave(&g.lock, flags);
	for (i = 0; i < MAX_EL; i++)
		out += g.ent[i].state == EL_OUT;
	seq_printf(m,"fixes session %d composer %d receiver %lu peek %lu canvas %lu register %lu el %u half %u\n",fix_session,composer,fixed_receiver,fixed_peek,fixed_canvas,fixed_register,READ_ONCE(*(u8 *)EL_FLAGS_ADDR),READ_ONCE(*(u8 *)(EL_FLAGS_ADDR+1)));
	seq_printf(m,"draining %d checks %u vd2_writes %lu vd2_changed %d\n",g.closing,g.drain_checks,vd2_route_writes,vd2_changed);
	seq_printf(m,"bl_detached %d retired %llu\n",g.bl_detached,g.el_retired);
	seq_printf(m,"seek_wait_ms %u extended %llu\n",seek_wait_ms,g.seek_wait_extended);
 seq_printf(m,"coherent_bl_fallback %d writes %lu\n",coherent_bl_fallback,fallback_mode_writes);
 seq_printf(m,"ab copies %d button %d presses %lu el_mute %d neutral %lu\n",ab_copies,ab_handler_reg,ab_presses,el_mute,ab_muted);
 seq_printf(m,"blank_seek_gaps %d pending %d epochs %llu checks %llu\n",blank_seek_gaps,g.gap_pending,g.gap_epochs,g.gap_blank_checks);
	seq_printf(m,"residual_unpaired %d hits %lu\n",residual_unpaired,residual_unpaired_hits);
	seq_printf(m,"residual_bypass %d hits %lu\n",residual_bypass,residual_checks_bypassed);
	seq_printf(m,"fel_composer_mode %d writes %lu changed %d\n",fel_composer_mode,composer_mode_writes,composer_mode_changed);
	seq_printf(m, "version %s active %d provider %d probes %d/%d\n", DVFEL_SC2_VERSION,
		   g.active, g.prov_reg, krp_dual_ok, kp_parse_ok);
	seq_printf(m, "el in %llu out %llu put %llu dropped %llu flushed %llu queued %d on_display %d\n",
		   g.el_in, g.el_out, g.el_put, g.el_dropped, g.el_flushed, g.nq, out);
	seq_printf(m, "owned_pair_rechecks %llu\n",g.bl_reused);
	seq_printf(m, "wait checks %llu ready %llu timeouts %llu\n", g.wait_checks, g.wait_ready, g.wait_timeouts);
	seq_printf(m, "bl paired %llu no_el %llu no_hint %llu no_crc %llu\n", g.bl_paired, g.bl_no_el,
		   g.bl_no_hint, g.bl_no_crc);
	seq_printf(m, "hints in %llu pending %d skipped %llu overflow %llu\n", g.hints_in, g.nh,
		   g.hints_skipped, g.hints_overflow);
	spin_unlock_irqrestore(&g.lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static int __init dvfel_init(void)
{
	int ret;

	if (!open_driver_fixed) {
		pr_err(DRV_NAME ": Gazelle open-driver integration not enabled; refusing to load candidate\n");
		return -EOPNOTSUPP;
	}

	/* the inner layout of src_fmt used here, as in the Dune binary (see the top) */
	BUILD_BUG_ON(offsetof(struct vframe_src_fmt_s, sei_ptr) != 8);
	BUILD_BUG_ON(offsetof(struct vframe_src_fmt_s, sei_size) != 16);
	BUILD_BUG_ON(offsetof(struct vframe_src_fmt_s, dual_layer) != 20);

	/* Only the offline-reviewed PS7717 layout is allowed in this candidate. */
	src_fmt_off = measure_src_fmt_off();
	if (src_fmt_off != SC2_VF_SRC_FMT_OFF)
		return -EINVAL;

	spin_lock_init(&g.lock);
	spin_lock_init(&g.fence_lock);
	g.fence_ctx = dma_fence_context_alloc(1);
	mutex_init(&g.sess_lock);
	mutex_init(&g.ioctl_lock);
	init_waitqueue_head(&g.done_wq);
	INIT_WORK(&g.rel_work, rel_work_fn);
	INIT_DELAYED_WORK(&g.orphan_work, orphan_work_fn);

	ret=fixes_init(); if(ret)return ret;
	krp_dual.kp.symbol_name = "is_dovi_dual_layer_frame";
	krp_dual.entry_handler = dual_entry;
	krp_dual.handler = dual_ret;
	krp_dual.data_size = sizeof(bool);
	krp_dual.maxactive = 4;
	ret = register_kretprobe(&krp_dual);
	if (ret) {
		pr_err(DRV_NAME ": kretprobe is_dovi_dual_layer_frame: %d\n", ret);
		fixes_exit(); return ret;
	}
	krp_dual_ok = true;
	kp_parse.symbol_name = "amdv_parse_metadata";
	kp_parse.pre_handler = parse_pre;
	ret = register_kprobe(&kp_parse);
	if (ret) {
		pr_err(DRV_NAME ": kprobe amdv_parse_metadata: %d\n", ret);
		goto err_krp;
	}
	kp_parse_ok = true;
	krp_wait.kp.symbol_name = "amdv_wait_metadata";
	krp_wait.entry_handler = wait_entry;
	krp_wait.handler = wait_ret;
	krp_wait.data_size = sizeof(struct vframe_s *);
	krp_wait.maxactive = 8;
	ret = register_kretprobe(&krp_wait);
	if (ret)
		goto err_kp;
	ret = misc_register(&dvfel_misc);
	if (ret)
		goto err_wait;
	dbg_dir = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("stats", 0444, dbg_dir, NULL, &stats_fops);
	src_fmt_off = measure_src_fmt_off();
	if (src_fmt_off < 0)
		pr_warn(DRV_NAME ": vframe layout of this firmware not found: no FEL pairing\n");
	else if (src_fmt_off != SC2_VF_SRC_FMT_OFF)
		pr_info(DRV_NAME ": vframe src_fmt at %d (Dune r24: %d)\n", src_fmt_off, SC2_VF_SRC_FMT_OFF);
	/* A/B copies the firmware's vframes: only when their layout is the headers' */
	if (src_fmt_off == (int)offsetof(struct vframe_s, src_fmt)) {
		WRITE_ONCE(ab_copies, true);
		if (input_register_handler(&ab_handler))
			pr_warn(DRV_NAME ": A/B button handler not registered\n");
		else
			ab_handler_reg = true;
	} else {
		pr_info(DRV_NAME ": vframe layout differs from the headers: no A/B button\n");
	}
	/* Do not alter Dolby driver state on module load. */
	pr_info(DRV_NAME ": SC2 %s loaded (src_fmt %d)\n", DVFEL_SC2_VERSION, src_fmt_off);
	return 0;

err_wait:
	unregister_kretprobe(&krp_wait);
err_kp:
	unregister_kprobe(&kp_parse);
err_krp:
	unregister_kretprobe(&krp_dual);
	fixes_exit();
	return ret;
}

static void __exit dvfel_exit(void)
{
	int i;

	debugfs_remove_recursive(dbg_dir);
	if (ab_handler_reg)
		input_unregister_handler(&ab_handler);
	cancel_work_sync(&ab_work);
	misc_deregister(&dvfel_misc);
	unregister_kretprobe(&krp_wait);
	unregister_kprobe(&kp_parse);
	unregister_kretprobe(&krp_dual);
	mutex_lock(&g.sess_lock);
	WRITE_ONCE(g.active, false);
	flush_queue(DVFEL_SC2_FLUSH_EL | DVFEL_SC2_FLUSH_HINTS);
	if (g.prov_reg) {
		vf_unreg_provider(&g.prov);
		g.prov_reg = false;
	}
	g.owner = NULL;
	mutex_unlock(&g.sess_lock);
	cancel_delayed_work_sync(&g.orphan_work);
	fixes_exit();
	/* Module pins prevent unloading with any outstanding display frames. */
	for (i = 0; i < MAX_EL; i++)
		if (g.ent[i].state == EL_OUT)
			g.ent[i].state = EL_RELEASE;
	rel_work_fn(NULL);
	cancel_work_sync(&g.rel_work);
	mute_free();
}

module_init(dvfel_init);
module_exit(dvfel_exit);
MODULE_DESCRIPTION("Dolby Vision FEL by the Dolby Vision core of Amlogic SC2 (Android)");
MODULE_AUTHOR("djnice");
MODULE_LICENSE("GPL");
MODULE_VERSION(DVFEL_SC2_VERSION);
