// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3d: the Android UI over both eyes of a frame packed session (ui_3d=1).
 *
 * Frame packed 1080p is a 1920x2205 frame (left eye lines 0-1079, 45 blank lines, right eye 1125-2204) set
 * behind Android's back: Android still composes its UI at 1920x1080. On Fire OS 7.7.1.4 the composer shows
 * the UI through the framebuffer OSD driver in its HWC (layers) mode, not through DRM: each frame is an
 * osd_layers_fence_map_s whose disp_info places the composed background (1920x1080) on the screen, and
 * osd_setting_blend() scales it by position / background in the OSD free scaler. Here, when the OSD driver
 * takes a frame (_osd_pan_display_layers_fence), its position is stretched by 2205/1080, so the UI covers the
 * whole frame: a player that draws its UI once per eye - rows 0-529 for the left eye, rows 551-1080 for the
 * right one, as Kodi does with its GUI in frame packed mode at full height - shows menus and subtitles in 3D.
 * Only for a player that asks for it (MVC3D_HW_F_UI3D in its START): for any other player, and with
 * ui_3d=0, the UI is hidden for the session, as before.
 *
 * (Measured 2026-11-26: over 4 s of UI updates, 130 frames through osd_sync_request_render and
 * osd_toggle_buffer, none through DRM's atomic commit, where the .17/.18 hook was.)
 *
 * struct osd_layers_fence_map_s / display_flip_info_s from the 7.7.1.4 OSD headers; the values are checked
 * before anything is written.
 */
#include "mvc3d.h"
#include "osd/osd.h"

static int ui_3d = 1;
module_param(ui_3d, int, 0644);
MODULE_PARM_DESC(
	ui_3d,
	"1: during a frame packed 1080p session the Android UI spans both eyes (a 3D-aware player draws it per eye); 0: hidden");

static int ui_base_h, ui_out_h;	/* 1080, 2205 */
static bool ui_probe_on, ui_layout_bad;
static bool ui_wanted;	/* this session's player draws its UI per eye (it asked: MVC3D_HW_F_UI3D) */
static u64 ui_stretched, ui_other;

/*
 * A frame's display info, checked by its values: 1 to stretch, 0 nothing to do (not a 1080-line UI on the
 * 1080-line screen, or already stretched), -1 not the expected layout
 */
static int disp_info_check(const struct display_flip_info_s *d)
{
	if (!d->background_w || d->background_w > 4096 || !d->background_h || d->background_h > 4096 ||
	    !d->position_w || d->position_w > 8192 || !d->position_h || d->position_h > 8192 ||
	    d->position_x > 8192 || d->position_y > 8192)
		return -1;
	if (d->background_h != (u32)ui_base_h || d->position_y + d->position_h > (u32)ui_base_h)
		return 0;
	return 1;
}

/* _osd_pan_display_layers_fence(output_index, vinfo, fence_map): x0, x1, x2; in the buffer toggle worker */
static int ui_pan_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct osd_layers_fence_map_s *fm = (void *)regs->regs[2];
	struct display_flip_info_s *d;
	u32 y, h;
	int r;

	if (regs->regs[0] != VIU1 || !READ_ONCE(mvc3d_fp_on) || !READ_ONCE(ui_wanted) || !READ_ONCE(ui_3d) ||
	    ui_layout_bad || !ui_base_h || !fm || !virt_addr_valid(fm))
		return 0;
	d = &fm->disp_info;
	r = disp_info_check(d);
	if (r < 0) {
		ui_layout_bad = true;
		pr_warn("ui_3d: OSD frame info not as expected (background %ux%u, position %u,%u %ux%u): UI not stretched\n",
			d->background_w, d->background_h, d->position_x, d->position_y, d->position_w,
			d->position_h);
		return 0;
	}
	if (!r) {
		ui_other++;
		return 0;
	}
	y = d->position_y * ui_out_h / ui_base_h;
	h = (d->position_y + d->position_h) * ui_out_h / ui_base_h - y;
	if (!ui_stretched)
		pr_info("ui_3d: UI over both eyes (OSD background %ux%u, position %u,%u %ux%u -> y %u h %u)\n",
			d->background_w, d->background_h, d->position_x, d->position_y, d->position_w,
			d->position_h, y, h);
	d->position_y = y;
	d->position_h = h;
	ui_stretched++;
	return 0;
}

static struct kprobe ui_probe = {
	.symbol_name = "_osd_pan_display_layers_fence",
	.pre_handler = ui_pan_pre,
};

/*
 * (frame packing hooks lock held) on with the session's frame packing hooks: base_h x out_h the frame
 * packed format (1080 x 2205); only 1080p frame packing, where Android's UI is 1080 lines
 */
void mvc3d_ui3d_hooks(bool on, int base_h, int out_h)
{
	int r;

	if (!on) {
		if (ui_probe_on) {
			unregister_kprobe(&ui_probe);
			if (ui_stretched || ui_other)
				pr_info("ui_3d: %llu frames over both eyes, %llu left as they were\n",
					ui_stretched, ui_other);
		}
		ui_probe_on = false;
		WRITE_ONCE(ui_wanted, false);
		return;
	}
	if (ui_probe_on || !READ_ONCE(ui_3d) || base_h != 1080 || out_h <= base_h)
		return;
	ui_base_h = base_h;
	ui_out_h = out_h;
	ui_stretched = 0;
	ui_other = 0;
	ui_probe.addr = NULL;
	ui_probe.flags = 0;
	r = register_kprobe(&ui_probe);
	ui_probe_on = !r;
	if (r)
		pr_warn("ui_3d: no probe on _osd_pan_display_layers_fence (%d): UI hidden in 3D\n", r);
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
