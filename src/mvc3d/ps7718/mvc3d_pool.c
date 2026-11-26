// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3d: the MVC decoder's work space and reference buffers below 2 GiB.
 * PS7718 dependencies: see mvc3d.h.
 */

#include "mvc3d.h"

/* ------------------------------------------------------------------ */
/* the decoder's 31 bit DMA: its work space and reference buffers     */
/* ------------------------------------------------------------------ */

/*
 * The firmware's amvdec_h264mvc drops bit 31 of some of its DMA write addresses: its work space and
 * its two reference (co-located motion vector) buffers get part of their data 2 GiB lower, in
 * whatever owns that memory (measured 11-05 with hwmvc/sc2/memcanary + the allocation log:
 * 0xd0a20000 -> 0x50a20000; other processes' pages, the page cache: crashes, a kernel oops). The
 * codec CMA lies at 0xcc000000-0xe6ffffff, so it always happens. Its pictures are written right
 * (bit exact).
 *
 * So these three buffers are moved below 2 GiB, where the address is the same with or without bit
 * 31: at MVC3D_HW_START a pool is taken there with alloc_contig_range() (movable pages are migrated
 * away), and the decoder's decoder_bmmu_box_alloc_buf_phy() for them (the work space: index 0; a
 * reference buffer: the first allocation in init_canvas()) returns an address in it. The decoder's
 * own buffer stays allocated, unused. The pool is freed once no decoder is left. No pool: no
 * hardware MVC.
 */
/* work space + both views' reference buffers, up to 16 frames each (8 frames: 13.1 MB) */
#define POOL_SIZE (28 << 20)
#define POOL_BOTTOM 0x31000000UL /* above the low reserved areas (dsp firmware at 0x30000000) */

static unsigned long pool_pfn; /* 0: none */
static unsigned long pool_used; /* bytes handed out to the current decoder */
static atomic_t pool_redirects = ATOMIC_INIT(0), pool_misses = ATOMIC_INIT(0);
#define POOL_SLOTS 16
static struct {
	int idx;
	unsigned long off, len;
} pool_slot[POOL_SLOTS];
static int pool_slots;
static bool pool_same_decoder; /* the current decoder's work space is in */
static atomic_t ref_next =
	ATOMIC_INIT(0); /* init_canvas entered: its next allocation is the reference buffer */
static DEFINE_SPINLOCK(pool_lock);

int mvc3d_pool_get(struct device *dev)
{
	unsigned long base, pfn, n = POOL_SIZE >> PAGE_SHIFT;
	int tries = 0;

	if (pool_pfn)
		return 0;
	for (base = POOL_TOP - POOL_SIZE; base >= POOL_BOTTOM && tries < 48;
	     base -= 4 << 20, tries++) {
		pfn = base >> PAGE_SHIFT;
		if (alloc_contig_range(pfn, pfn + n, MIGRATE_MOVABLE, GFP_KERNEL))
			continue;
		/* clean: no dirty cache lines over what the decoder writes */
		memset(phys_to_virt(base), 0, POOL_SIZE);
		dma_sync_single_for_device(dev, base, POOL_SIZE, DMA_TO_DEVICE);
		pool_pfn = pfn;
		pool_used = 0;
		pr_info("low DMA pool %#lx..%#lx (%d tries)\n", base, base + POOL_SIZE, tries + 1);
		return 0;
	}
	pr_warn("no %d MiB below 2 GiB for the MVC decoder (%d tries): hardware MVC refused\n",
		POOL_SIZE >> 20, tries);
	return -ENOMEM;
}

void mvc3d_pool_put(void)
{
	if (!pool_pfn)
		return;
	free_contig_range(pool_pfn, POOL_SIZE >> PAGE_SHIFT);
	pr_info("low DMA pool freed (%d buffers moved, %d not)\n", atomic_read(&pool_redirects),
		atomic_read(&pool_misses));
	pool_pfn = 0;
	pool_used = 0;
	pool_slots = 0;
	pool_same_decoder = false;
}

static int ref_pre(struct kprobe *p, struct pt_regs *regs)
{
	atomic_set(&ref_next, 1);
	return 0;
}

static struct kprobe ref_probe = {
	.symbol_name = "amvdec_h264mvc:init_canvas",
	.pre_handler = ref_pre,
};
static bool ref_probe_on;

/* decoder_bmmu_box_alloc_buf_phy(handle, idx, size, driver_name, &phys) */
struct alloc_ctx {
	int idx, size, move;
	unsigned long *addr;
};

static int alloc_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct alloc_ctx *c = (struct alloc_ctx *)ri->data;
	const char *name = (const char *)regs->regs[3];
	char nm[16] = "";

	if (!name || probe_kernel_read(nm, name, sizeof(nm) - 1) ||
	    strncmp(nm, "amvdec_h264mvc", 14))
		return 1; /* another decoder: nothing to do on return */
	c->idx = regs->regs[1];
	c->size = regs->regs[2];
	c->addr = (unsigned long *)regs->regs[4];
	c->move = c->idx == 0 || atomic_xchg(&ref_next, 0);
	return 0;
}

static int alloc_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct alloc_ctx *c = (struct alloc_ctx *)ri->data;
	unsigned long flags, orig = c->addr ? *c->addr : 0, to = 0;

	if (c->move && !regs_return_value(regs) && c->addr && c->size > 0) {
		unsigned long need = PAGE_ALIGN(c->size);

		int i;

		spin_lock_irqsave(&pool_lock, flags);
		/* the work space comes first: a new decoder (the decoder is single instance) */
		if (c->idx == 0 && !pool_same_decoder) {
			pool_used = 0;
			pool_slots = 0;
			pool_same_decoder = true;
		}
		/*
		 * the decoder asks again for what it has, at every new SPS (avatar.mk3d, 11-06):
		 * the box gives the same buffer, so the same place here too - a new one each time
		 * ran the pool out, and a buffer stayed above 2 GiB (kernel and user memory
		 * overwritten)
		 */
		for (i = 0; i < pool_slots; i++) {
			if (pool_slot[i].idx == c->idx && need <= pool_slot[i].len) {
				to = (pool_pfn << PAGE_SHIFT) + pool_slot[i].off;
				break;
			}
		}
		if (!to && pool_pfn && pool_used + need <= POOL_SIZE && pool_slots < POOL_SLOTS) {
			to = (pool_pfn << PAGE_SHIFT) + pool_used;
			pool_slot[pool_slots].idx = c->idx;
			pool_slot[pool_slots].off = pool_used;
			pool_slot[pool_slots].len = need;
			pool_slots++;
			pool_used += need;
		}
		spin_unlock_irqrestore(&pool_lock, flags);
		if (to) {
			*c->addr = to;
			atomic_inc(&pool_redirects);
		} else if (orig >= POOL_TOP) {
			/* never into high memory: the decoder's init fails instead */
			atomic_inc(&pool_misses);
			pr_err("MVC decoder buffer %d (%#x bytes) at %#lx cannot go below 2 GiB: refused\n",
			       c->idx, c->size, orig);
			regs_set_return_value(regs, -ENOMEM);
		}
	}
	if (READ_ONCE(mvc3d_debug))
		pr_info("alloc idx %d size %#x at %#lx%s%s\n", c->idx, c->size, orig,
			to ? " -> " : "", to ? "low pool" : "");
	return 0;
}

static struct kretprobe alloc_probe = {
	.kp.symbol_name = "decoder_bmmu_box_alloc_buf_phy",
	.entry_handler = alloc_entry,
	.handler = alloc_ret,
	.data_size = sizeof(struct alloc_ctx),
	.maxactive = 16,
};
static bool alloc_probe_on;

/* the pool goes when no decoder can write into it any more (h->mlock held) */
void mvc3d_pool_release_if_idle(struct sc2_dev *h)
{
	if (!h->started && !READ_ONCE(h->prov_reg))
		mvc3d_pool_put();
}

static void pool_work_fn(struct work_struct *w)
{
	mutex_lock(&mvc3d_dev->mlock);
	mvc3d_pool_release_if_idle(mvc3d_dev);
	mutex_unlock(&mvc3d_dev->mlock);
}
static DECLARE_WORK(pool_work, pool_work_fn);

/* the decoder unregistered (its buffers are free): the pool is free for the next one, or goes */
void mvc3d_pool_decoder_gone(void)
{
	unsigned long flags;

	spin_lock_irqsave(&pool_lock, flags);
	pool_used = 0;
	pool_slots = 0;
	pool_same_decoder = false;
	spin_unlock_irqrestore(&pool_lock, flags);
	schedule_work(&pool_work);
}

void mvc3d_pool_reset(void)
{
	pool_used = 0;
	pool_slots = 0;
	pool_same_decoder = false;
}

bool mvc3d_pool_probes_on(void)
{
	return ref_probe_on && alloc_probe_on;
}

/*
 * The probes on the MVC decoder's allocations are only registered while a session runs (from its
 * start until its stop): idle, the module is not on the decoders' paths. A probe given by name keeps
 * the address register_kprobe() looked up, which a second registration refuses: cleared first.
 */
int mvc3d_pool_hooks(bool on)
{
	int r;

	if (!on) {
		if (ref_probe_on)
			unregister_kprobe(&ref_probe);
		if (alloc_probe_on)
			unregister_kretprobe(&alloc_probe);
		ref_probe_on = false;
		alloc_probe_on = false;
		return 0;
	}
	if (ref_probe_on && alloc_probe_on)
		return 0;
	ref_probe.addr = NULL;
	ref_probe.flags = 0;
	r = register_kprobe(&ref_probe);
	ref_probe_on = !r;
	if (r) {
		pr_warn("no probe on amvdec_h264mvc init_canvas (%d): hardware MVC refused\n", r);
		return -ENODEV;
	}
	alloc_probe.kp.addr = NULL;
	alloc_probe.kp.flags = 0;
	r = register_kretprobe(&alloc_probe);
	alloc_probe_on = !r;
	if (r) {
		pr_warn("no probe on decoder_bmmu_box_alloc_buf_phy (%d): hardware MVC refused\n", r);
		mvc3d_pool_hooks(false);
		return -ENODEV;
	}
	return 0;
}

void mvc3d_pool_init(void)
{
}

void mvc3d_pool_exit(void)
{
	mvc3d_pool_hooks(false);
	cancel_work_sync(&pool_work);
}

void mvc3d_pool_stats(struct seq_file *m)
{
	seq_printf(
		m,
		"decoder probes %s, low pool %#lx (%lu KiB used), buffers moved %d, not moved %d\n",
		ref_probe_on && alloc_probe_on ? "on" : "OFF", pool_pfn << PAGE_SHIFT,
		pool_used >> 10, atomic_read(&pool_redirects), atomic_read(&pool_misses));
}
