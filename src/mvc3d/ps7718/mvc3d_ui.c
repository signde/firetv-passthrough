// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3d: the Android UI over both eyes of a frame packed session (ui_3d=1).
 *
 * Frame packed 1080p is a 1920x2205 frame (left eye lines 0-1079, 45 blank lines, right eye 1125-2204) set
 * behind Android's back: Android still composes its UI at 1920x1080, and the DRM plane code clips each UI
 * plane to that 1080-line mode. Here the planes' destinations are stretched by 2205/1080 when the VPU
 * pipeline is worked out (vpu_pipeline_traverse: the OSD scalers and blend windows come from them), so the
 * UI covers the whole frame: a player that draws its UI once per eye - rows 0-529 for the left eye, rows
 * 551-1080 for the right one, as Kodi does with its GUI in frame packed mode at full height - shows menus
 * and subtitles in 3D. Only for a player that asks for it (MVC3D_HW_F_UI3D in its START): for any other
 * player, and with ui_3d=0, the UI is hidden for the session, as before.
 *
 * struct meson_vpu_pipeline_state / meson_vpu_osd_layer_info from the 7.7.1.4 headers; checked by their
 * values before use.
 */
#include "mvc3d.h"
#include "meson_vpu_pipeline.h"

static int ui_3d = 1;
module_param(ui_3d, int, 0644);
MODULE_PARM_DESC(
	ui_3d,
	"1: during a frame packed 1080p session the Android UI spans both eyes (a 3D-aware player draws it per eye); 0: hidden");

static int ui_base_h, ui_out_h;	/* 1080, 2205 */
static bool ui_probe_on, ui_layout_bad;
static bool ui_wanted;	/* this session's player draws its UI per eye (it asked: MVC3D_HW_F_UI3D) */
static u64 ui_stretched;

/* what this hook wrote last per plane, so a second pass over the same state does not stretch twice */
static struct {
	void *mvps;
	int y;
	u32 h;
} ui_last[MESON_MAX_OSDS];

/*
 * The 7.7.1.4 layout, checked by values before anything is written: every slot's enable is 0/1; an
 * enabled slot is the plane meson_plane_atomic_check() just filled in: its plane_index is its slot,
 * its source a real buffer's (1-4096 wide and high, inside the buffer).
 */
/* 1: sane, 0: no enabled plane (nothing to do), -1: not the expected layout */
static int layout_sane(struct meson_vpu_pipeline_state *mvps)
{
	int i, enabled = 0;

	for (i = 0; i < MESON_MAX_OSDS; i++) {
		struct meson_vpu_osd_layer_info *p = &mvps->plane_info[i];

		if (p->enable > 1)
			return -1;
		if (!p->enable)
			continue;
		enabled++;
		if (p->plane_index != i || !p->src_w || p->src_w > 4096 || !p->src_h ||
		    p->src_h > 4096 || p->dst_w > 8192 || p->dst_h > 8192 ||
		    (p->fb_w && p->src_w > p->fb_w))
			return -1;
	}
	return enabled > 0;
}

/* vpu_pipeline_traverse(mvps, state): x0 = mvps */
static int ui_traverse_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct meson_vpu_pipeline_state *mvps = (void *)regs->regs[0];
	int i, r;

	if (!READ_ONCE(mvc3d_fp_on) || !READ_ONCE(ui_wanted) || !READ_ONCE(ui_3d) || ui_layout_bad ||
	    !mvps || !ui_base_h)
		return 0;
	r = layout_sane(mvps);
	if (!r)
		return 0;
	if (r < 0) {
		ui_layout_bad = true;
		pr_warn("ui_3d: VPU pipeline state not the 7.7.1.4 layout: UI not stretched\n");
		return 0;
	}
	for (i = 0; i < MESON_MAX_OSDS; i++) {
		struct meson_vpu_osd_layer_info *p = &mvps->plane_info[i];
		int y;
		u32 h;

		if (!p->enable || !p->dst_h)
			continue;
		if (ui_last[i].mvps == mvps && ui_last[i].y == p->dst_y && ui_last[i].h == p->dst_h)
			continue;
		if (p->dst_y < 0 || p->dst_y + p->dst_h > (u32)ui_base_h)
			continue;
		y = p->dst_y * ui_out_h / ui_base_h;
		h = (p->dst_y + p->dst_h) * ui_out_h / ui_base_h - y;
		p->dst_y = y;
		p->dst_h = h;
		ui_last[i].mvps = mvps;
		ui_last[i].y = y;
		ui_last[i].h = h;
		ui_stretched++;
	}
	return 0;
}

static struct kprobe ui_probe = {
	.symbol_name = "vpu_pipeline_traverse",
	.pre_handler = ui_traverse_pre,
};

/*
 * (frame packing hooks lock held) on with the session's frame packing hooks: base_h x out_h the frame
 * packed format (1080 x 2205); only 1080p frame packing, where Android's UI is 1080 lines
 */
void mvc3d_ui3d_hooks(bool on, int base_h, int out_h)
{
	int r;

	if (!on) {
		if (ui_probe_on)
			unregister_kprobe(&ui_probe);
		ui_probe_on = false;
		WRITE_ONCE(ui_wanted, false);
		return;
	}
	if (ui_probe_on || !READ_ONCE(ui_3d) || base_h != 1080 || out_h <= base_h)
		return;
	ui_base_h = base_h;
	ui_out_h = out_h;
	memset(ui_last, 0, sizeof(ui_last));
	ui_stretched = 0;
	ui_probe.addr = NULL;
	ui_probe.flags = 0;
	r = register_kprobe(&ui_probe);
	ui_probe_on = !r;
	if (r)
		pr_warn("ui_3d: no probe on vpu_pipeline_traverse (%d): UI hidden in 3D\n", r);
}

/*
 * (process context) whether the session's player asked for the UI over both eyes: set at its start
 * before the display is taken, cleared once the display is back
 */
void mvc3d_ui3d_want(bool want)
{
	WRITE_ONCE(ui_wanted, want);
}

/* whether the UI could span both eyes this session (the probe on, the layout as expected) */
bool mvc3d_ui3d_ready(void)
{
	return ui_probe_on && READ_ONCE(ui_3d) && !ui_layout_bad;
}

/* whether this session's UI spans both eyes (else the session hides it) */
bool mvc3d_ui3d_active(void)
{
	return mvc3d_ui3d_ready() && READ_ONCE(ui_wanted);
}

u64 mvc3d_ui3d_stretched(void)
{
	return ui_stretched;
}
