// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3d: HDMI frame packing on the T7 hdmitx21 (Fire OS PS7718), the 3D VSIF and the video layer
 * fixes. PS7718 dependencies: see mvc3d.h.
 */

#include "mvc3d.h"

/* ------------------------------------------------------------------ */
/* HDMI frame packing on the T7 (gazelle, hdmitx21)                    */
/* ------------------------------------------------------------------ */

/*
 * hdmitx21 (PS7718 binary, checked against the 7.7.1.3 source and disassembly 2026-11-25):
 *  - the clock: hdmitx21_set_clk() takes setting_3dfp_enc_clk_val[] when hdev->flag_3dfp is set:
 *    VIC 32/33/34 with od1 2 instead of 4 = 148.5 MHz, 24 bit colour only (so the mode set is
 *    forced to 444,8bit);
 *  - the encoder: config_tv_enc_calc() (in set_tv_encp_new) programs ENCP from
 *    hdmitx21_gettiming_from_vic(): there is no 3D table, so while frame packed that lookup gives a
 *    copy of the mode with 2250 lines, 2205 active (45 blank, same porches/sync), 148.5 MHz;
 *  - para/vinfo: hdmitx21_get_fmtpara() (the lookup by name inlined) fills hdev->para->timing,
 *    tmds_clk (the PHY setting) and hdmitx_vinfo (the VPP's output size) - patched on return to the
 *    same frame packed timing (height 2205, vtotal 2250, 148500 kHz), so the VPP puts out 2205
 *    lines;
 *  - the VSIF: our own HDMI 1.4b 3D VSIF (OUI 00 0c 03, PB4 0x40, PB5 3D_Structure 0
 *    = frame packing, the rest zero: hdmi21_set_3d leaves db[7..27] uninitialised and tpi_info_send
 *    checksums all 31 bytes) by hdmi_vend_infoframe_rawset(), in the mode set's own thread: at the
 *    fmtpara entry (with the flag, before the clock and encoder), again when hdmitx21_set_display
 *    returns, and whenever a vendor infoframe write (_tpi_infoframe_wrrd, all of them pass there:
 *    hdmitx_set_hdr10plus_pkt "null vsif", the DV and HDR10+ code) replaces or clears it while
 *    frame packed, right after that write. No sysfs ("3dfp" from a kernel worker is denied by
 *    SELinux).
 *  - 720p60/50 too (VIC 4/19, 1280x1470, 1500 lines, 30 blank, 148.5 MHz: the same 3dfp clock row).
 * get_fmtpara is called by hdmitx_validate_vmode (vinfo for vout), hdmitx_set_current_vmode (the
 * mode set, set_disp_mode_auto inlined, both the vout and the DRM path) and by sysfs readers
 * (valid_mode, disp_cap ...): the flag/attr state changes only on calls from
 * hdmitx_set_current_vmode. hdmitx_dev offsets (fmt_attr +10192, para +184, flag_3dfp +10536 bit 0)
 * are the 7.7.1.3 header layout, matched in the PS7718 binary (set_clk: ldrb [x25, #0x2928] / tbnz
 * #0; set_current_vmode: x1 = &fmt_attr).
 */

static struct hdmitx_dev *(*p_get_hdmitx21_device)(void);
static struct vinfo_s *(*p_get_current_vinfo)(void);
#define get_current_vinfo() p_get_current_vinfo()

bool mvc3d_fp_ok; /* layout checked, probes on */
static int fp_win_h = 2205, fp_win_w = 1920, fp_base_w = 1920,
	   fp_base_h = 1080; /* the frame packed frame (video window) */
bool mvc3d_fp_armed; /* a client plays frame packed */
bool mvc3d_fp_on; /* the last mode set was frame packed (the hdmitx has our 3D flag) */
static char fp_attr_saved[16];
static bool fp_attr_forced;
static u64 fp_modesets, fp_maps;
static char fp_dv_policy[8], fp_dv_mode[8];
static bool fp_dv_saved;

/* the frame packed timings: 1080p24/25/30 (VIC 32/33/34), 720p60/50 (VIC 4/19) */
#define FP_N 5
static struct hdmi_timing fp_timing[FP_N];
static const enum hdmi_vic fp_vics[FP_N] = { HDMI_32_1920x1080p24_16x9, HDMI_33_1920x1080p25_16x9,
					     HDMI_34_1920x1080p30_16x9, HDMI_4_1280x720p60_16x9,
					     HDMI_19_1280x720p50_16x9 };
/* the 2D mode: width, height, lines, blank lines */
static const u16 fp_base[FP_N][4] = { { 1920, 1080, 1125, 45 },
				      { 1920, 1080, 1125, 45 },
				      { 1920, 1080, 1125, 45 },
				      { 1280, 720, 750, 30 },
				      { 1280, 720, 750, 30 } };
static int fp_cur = -1; /* the fp_timing[] of the current frame packed mode set */

static int fp_mode_index(const char *m)
{
	static const char *const n[FP_N][2] = { { "1080p24hz", "1920x1080p24hz" },
						{ "1080p25hz", "1920x1080p25hz" },
						{ "1080p30hz", "1920x1080p30hz" },
						{ "720p60hz", "1280x720p60hz" },
						{ "720p50hz", "1280x720p50hz" } };
	int i;

	for (i = 0; i < FP_N; i++)
		if (!strcmp(m, n[i][0]) || !strcmp(m, n[i][1]))
			return i;
	return -1;
}

/*
 * from the firmware's own timing: same horizontal timing, 2 * the lines, 2 * active + blank, twice
 * the clock
 */
static bool fp_make_timing(int i, const struct hdmi_timing *t)
{
	struct hdmi_timing *f = &fp_timing[i];
	const u16 *b = fp_base[i];

	if (!t || t->vic != fp_vics[i] || t->h_active != b[0] || t->v_active != b[1] ||
	    t->v_total != b[2] || t->v_blank != b[3] || t->pixel_freq != 74250 || !t->pi_mode)
		return false;
	*f = *t;
	f->v_total = 2 * b[2];
	f->v_active = 2 * b[1] + b[3];
	f->pixel_freq = 148500;
	f->h_freq = t->h_freq * 2;
	return true;
}

/*
 * the caller: a mode set = hdmitx_set_current_vmode (two static copies in the PS7718 kernel,
 * set_disp_mode_auto inlined) or set_disp_mode_auto itself (the HPD replug re-set)
 */
static bool from_setvmode(unsigned long ret)
{
	char sym[KSYM_SYMBOL_LEN];

	sprint_symbol_no_offset(sym, ret);
	return !strcmp(sym, "hdmitx_set_current_vmode") || !strcmp(sym, "set_disp_mode_auto");
}

static void fp_attr(struct hdmitx_dev *hd, bool force)
{
	if (force) {
		if (!fp_attr_forced) {
			memcpy(fp_attr_saved, hd->fmt_attr, sizeof(fp_attr_saved));
			fp_attr_forced = true;
		}
		memset(hd->fmt_attr, 0, sizeof(hd->fmt_attr));
		memcpy(hd->fmt_attr, "444,8bit", 8);
	} else if (fp_attr_forced) {
		memcpy(hd->fmt_attr, fp_attr_saved, sizeof(hd->fmt_attr));
		fp_attr_forced = false;
	}
}

static void fp_flag(struct hdmitx_dev *hd, bool on)
{
	hd->flag_3dtb = 0;
	hd->flag_3dss = 0;
	hd->flag_3dfp = on;
}

/* the 3D VSIF */
static void (*p_vend_rawset)(u8 *hb, u8 *pb);
static int fp_vsif_len = 5;
module_param(fp_vsif_len, int, 0644);
MODULE_PARM_DESC(
	fp_vsif_len,
	"frame packing: the 3D VSIF length (5: PB1-PB5 as HDMI 1.4b; 6: as the Amlogic driver)");
static u64 fp_vsif_sent, fp_vsif_reasserts, fp_vsif_clears;
static atomic_t fp_vsif_busy = ATOMIC_INIT(0);

/*
 * where the 3D VSIF goes. hdmi_vend_infoframe_rawset() picks the packet buffer by the
 * sink's EDID (Dolby Vision CTS rules): a sink without an InfoFrame Data Block (HDMI 1.4 3D sinks)
 * gets it in the HF-VSIF buffer (0x181). So: always the classic vendor buffer (0x81) and the
 * HF-VSIF buffer cleared, as hdmitx20's hdmi_set_3d() and CoreELEC common_drivers PR #32 ("Send
 * classic 3d vsif on the classic vendor slot") do.
 */
static void (*p_infoframe_send)(u16 info_type, u8 *body);

static void fp_vsif_send(void)
{
	u8 hb[3] = { 0x81, 0x01, 0x05 }, pb[28] = { 0 };

	if (p_infoframe_send) {
		u8 body[31] = { 0 };

		body[0] = 0x81;
		body[1] = 0x01;
		body[2] = READ_ONCE(fp_vsif_len) == 6 ? 6 : 5;
		body[4] = 0x03; /* PB1-3: IEEE OUI 00 0c 03, LSB first */
		body[5] = 0x0c;
		body[6] = 0x00;
		body[7] = 0x40; /* PB4: HDMI_Video_Format 3D */
		body[8] = 0x00; /* PB5: 3D_Structure 0, frame packing */
		atomic_inc(&fp_vsif_busy);
		p_infoframe_send(0x181, NULL);
		p_infoframe_send(0x81, body);
		atomic_dec(&fp_vsif_busy);
		fp_vsif_sent++;
		return;
	}
	if (!p_vend_rawset)
		return;
	hb[2] = READ_ONCE(fp_vsif_len) == 6 ? 6 : 5;
	pb[1] = 0x03; /* IEEE OUI 00 0c 03, LSB first */
	pb[2] = 0x0c;
	pb[3] = 0x00;
	pb[4] = 0x40; /* HDMI_Video_Format: 3D */
	pb[5] = 0x00; /* 3D_Structure 0: frame packing */
	atomic_inc(&fp_vsif_busy);
	p_vend_rawset(hb, pb);
	atomic_dec(&fp_vsif_busy);
	fp_vsif_sent++;
}

static void fp_vsif_clear(void)
{
	if (p_infoframe_send) {
		atomic_inc(&fp_vsif_busy);
		p_infoframe_send(0x81, NULL);
		atomic_dec(&fp_vsif_busy);
		fp_vsif_clears++;
		return;
	}
	if (!p_vend_rawset)
		return;
	atomic_inc(&fp_vsif_busy);
	p_vend_rawset(NULL, NULL);
	atomic_dec(&fp_vsif_busy);
	fp_vsif_clears++;
}

/* is this infoframe body our 3D frame packing VSIF */
static bool fp_vsif_is_ours(const u8 *body)
{
	u8 b[9];

	if (!body || probe_kernel_read(b, body, sizeof(b)))
		return false;
	return b[0] == 0x81 && b[4] == 0x03 && b[5] == 0x0c && b[6] == 0x00 &&
	       (b[7] & 0xe0) == 0x40 && (b[8] >> 4) == 0;
}

/*
 * _tpi_infoframe_wrrd(wr, info_type, body) (static, LTO name): every infoframe write; a vendor one
 * (0x81 bank 5, 0x181 bank 8) that is not ours while frame packed: ours again right after it
 */
static int wrrd_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	u16 type = regs->regs[1] & 0xffff;

	if (!READ_ONCE(mvc3d_fp_on) || !(regs->regs[0] & 0xff) || (type != 0x81 && type != 0x181) ||
	    atomic_read(&fp_vsif_busy) || fp_vsif_is_ours((const u8 *)regs->regs[2]))
		return 1;
	return 0;
}

static int wrrd_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	if (READ_ONCE(mvc3d_fp_on)) {
		fp_vsif_send();
		fp_vsif_reasserts++;
	}
	return 0;
}

static struct kretprobe wrrd_probe = {
	.entry_handler = wrrd_entry,
	.handler = wrrd_ret,
	.maxactive = 8,
};
static bool wrrd_probe_on;

static struct delayed_work fp_work;
static bool fp_work_on;

/*
 * the 3D VSIF kept: while frame packed, every second the classic vendor packet buffer
 * is read back (a read is not a write: wrrd_probe skips it) - not enabled or not ours: sent again;
 * an HF-VSIF enabled meanwhile: cleared by the send. Also right after an HDCP 2 (re-)authentication
 * (set_hdcp2_topo): the projector dropped to 2D at a sink requested re-auth (17:59:05).
 */
#define FP_VSIF_CHECK_MS 1000
static int (*p_infoframe_rawget)(u16 info_type, u8 *body);
static struct delayed_work fp_keep_work;
static bool fp_keep_ok, hdcp_probe_on;
static atomic_t fp_keep_stop = ATOMIC_INIT(0);
static u64 fp_vsif_checks, fp_vsif_lost, fp_hdcp_auths;
static bool fp_keep_force;

static bool (*p_is_amdv_on)(void);
static bool (*p_is_amdv_enable)(void);
/* the HDMI colour signalling (AVI, DRM infoframes, attr) and the video colour path registers */
static int colour_logged; /* 0, 1 (at 48 pictures), 2 (at 2400) - per session */
static void colour_dump(const char *when)
{
	static const u16 regs[] = {
		0x1d26, 0x1d5f, 0x3290, 0x3291, 0x3292, 0x3293, 0x3294, 0x3298, 0x3299, 0x329a,
		0x329b, 0x329c, 0x329d, 0x32b0, 0x32b1, 0x32b2, 0x32b3, 0x32b4, 0x32b8, 0x32b9,
		0x32ba, 0x32bb, 0x32bc, 0x32bd, 0x32c1, 0x39a0, 0x39a1, 0x39a2, 0x39a3, 0x39a4,
		0x39a8, 0x39a9, 0x39aa, 0x39ab, 0x39ac, 0x39ad, 0x3800, 0x383b, 0x383c, 0x3280,
		0x32a0, 0x1d6a, 0x1da1, 0x39d0, 0x39d4, 0x14b4,
	};
	u8 avi[32] = { 0 }, drm[32] = { 0 };
	int ra = -1, rd = -1, i;
	char buf[sizeof(regs) / 2 * 15 + 8];
	int n = 0;

	if (p_infoframe_rawget) {
		ra = p_infoframe_rawget(0x82, avi);
		rd = p_infoframe_rawget(0x87, drm);
	}
	pr_info("colour (%s): AVI %d %*ph; DRM %d %*ph; Dolby Vision enable %d on %d\n", when, ra,
		17, avi, rd, 8, drm, p_is_amdv_enable ? p_is_amdv_enable() : -1,
		p_is_amdv_on ? p_is_amdv_on() : -1);
	if (!mvc3d_read_vcbus)
		return;
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		n += scnprintf(buf + n, sizeof(buf) - n, " %04x=%08x", regs[i],
			       (u32)mvc3d_read_vcbus(regs[i]));
	pr_info("colour regs (%s):%s\n", when, buf);
}

static void fp_3d_type_keep(void);

static void fp_keep_fn(struct work_struct *w)
{
	int ms = FP_VSIF_CHECK_MS;

	if (READ_ONCE(mvc3d_debug) && mvc3d_dev && READ_ONCE(mvc3d_dev->started) && mvc3d_dev->fp &&
	    READ_ONCE(mvc3d_fp_on)) {
		u64 sh = READ_ONCE(mvc3d_dev->shown);

		if (colour_logged == 0 && sh >= 48) {
			colour_logged = 1;
			colour_dump("48 pictures");
		} else if (colour_logged == 1 && sh >= 2400) {
			colour_logged = 2;
			colour_dump("2400 pictures");
		}
	}

	if (atomic_read(&fp_keep_stop))
		return;
	if (mvc3d_dev && READ_ONCE(mvc3d_dev->started) && mvc3d_dev->fp && READ_ONCE(mvc3d_fp_on) &&
	    READ_ONCE(mvc3d_dev->shown))
		fp_3d_type_keep();
	if (READ_ONCE(mvc3d_fp_on) && p_infoframe_send && p_infoframe_rawget) {
		u8 b[32] = { 0 }, h2[32] = { 0 };
		int r = p_infoframe_rawget(0x81, b);
		int r2 = p_infoframe_rawget(0x181, h2);
		bool force = xchg(&fp_keep_force, false);

		fp_vsif_checks++;
		if (r <= 0 || !fp_vsif_is_ours(b) || r2 > 0 || force) {
			if (!force || r <= 0 || !fp_vsif_is_ours(b) || r2 > 0) {
				fp_vsif_lost++;
				pr_info("3D VSIF %s (vendor %d %02x %02x %02x %02x %02x, HF-VSIF %d)%s: sent again\n",
					r <= 0 ? "gone" : "replaced", r, b[0], b[4], b[5], b[6],
					b[7], r2, force ? " after HDCP auth" : "");
			}
			fp_vsif_send();
		}
	}
	if (ms > 0 || READ_ONCE(fp_keep_force))
		mod_delayed_work(system_wq, &fp_keep_work, msecs_to_jiffies(ms > 0 ? ms : 1000));
}

/* set_hdcp2_topo(1): HDCP 2 authenticated (also after a re-auth the sink asked for) */
static int hdcp_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (READ_ONCE(mvc3d_fp_on) && fp_keep_ok && !atomic_read(&fp_keep_stop)) {
		fp_hdcp_auths++;
		WRITE_ONCE(fp_keep_force, true);
		mod_delayed_work(system_wq, &fp_keep_work, msecs_to_jiffies(100));
	}
	return 0;
}

static struct kprobe hdcp_probe = {
	.symbol_name = "set_hdcp2_topo",
	.pre_handler = hdcp_pre,
};

struct fmtpara_call {
	int idx; /* the frame packed timing for this call, -1: none */
};

/* hdmitx21_get_fmtpara(mode, attr): entry */
static int fmtpara_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct fmtpara_call *c = (struct fmtpara_call *)ri->data;
	struct hdmitx_dev *hd = p_get_hdmitx21_device();
	bool setv = from_setvmode(regs->regs[30]);
	char m[24] = "";
	int idx;

	c->idx = -1;
	if (probe_kernel_read(m, (void *)regs->regs[0], sizeof(m) - 1))
		return 1;
	idx = READ_ONCE(mvc3d_fp_armed) ? fp_mode_index(m) : -1;
	if (setv) {
		/*
		 * the mode set: our flag and 8 bit 4:4:4 now, before the attr is parsed and the
		 * clock set
		 */
		fp_modesets++;
		if (idx >= 0) {
			fp_attr(hd, true);
			fp_flag(hd, true);
			WRITE_ONCE(fp_win_w, fp_base[idx][0]);
			WRITE_ONCE(fp_win_h, 2 * fp_base[idx][1] + fp_base[idx][3]);
			WRITE_ONCE(fp_base_w, fp_base[idx][0]);
			WRITE_ONCE(fp_base_h, fp_base[idx][1]);
		} else if (READ_ONCE(mvc3d_fp_on) || fp_attr_forced) {
			fp_flag(hd, false);
			fp_attr(hd, false);
		}
		WRITE_ONCE(fp_cur, idx);
		if ((idx >= 0) != READ_ONCE(mvc3d_fp_on) || idx >= 0) {
			bool was = READ_ONCE(mvc3d_fp_on);

			WRITE_ONCE(mvc3d_fp_on, idx >= 0);
			/* the VSIF with the flag, before the clock and the encoder change */
			if (idx >= 0)
				fp_vsif_send();
			else if (was)
				fp_vsif_clear();
			fp_work_on = idx >= 0;
			mod_delayed_work(system_wq, &fp_work,
					 msecs_to_jiffies(idx >= 0 ? 600 : 50));
		}
	}
	/*
	 * the validate call (vinfo for vout) and the mode set get the frame packed timing; sysfs
	 * readers not
	 */
	if (idx < 0)
		return 1;
	c->idx = idx;
	return 0;
}

static int fmtpara_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct fmtpara_call *c = (struct fmtpara_call *)ri->data;
	struct hdmi_format_para *p = (void *)regs_return_value(regs);
	struct hdmi_timing *f;
	struct vinfo_s *v;

	if (c->idx < 0 || !p)
		return 0;
	f = &fp_timing[c->idx];
	/* the firmware's values first: only the 2D mode of this VIC is changed */
	if (p->timing.vic != f->vic || p->timing.v_total != fp_base[c->idx][2] ||
	    p->timing.v_active != fp_base[c->idx][1] ||
	    p->hdmitx_vinfo.height != fp_base[c->idx][1] ||
	    p->hdmitx_vinfo.width != fp_base[c->idx][0] ||
	    p->hdmitx_vinfo.vtotal != fp_base[c->idx][2])
		return 0;
	p->timing = *f;
	if (p->cd == COLORDEPTH_24B && p->cs != HDMI_COLORSPACE_YUV420)
		p->tmds_clk = f->pixel_freq;
	v = &p->hdmitx_vinfo;
	v->height = f->v_active;
	v->field_height = f->v_active;
	v->vtotal = f->v_total;
	v->video_clk = f->pixel_freq;
	fp_maps++;
	return 0;
}

static struct kretprobe fmtpara_probe = {
	.kp.symbol_name = "hdmitx21_get_fmtpara",
	.entry_handler = fmtpara_entry,
	.handler = fmtpara_ret,
	.data_size = sizeof(struct fmtpara_call),
	.maxactive = 8,
};
static bool fmtpara_probe_on;

/* hdmitx21_gettiming_from_vic(vic): the encoder's timing while the mode set is frame packed */
static int tvic_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	int *idx = (int *)ri->data;
	int cur = READ_ONCE(fp_cur);

	*idx = -1;
	if (!READ_ONCE(mvc3d_fp_on) || cur < 0 || (u32)regs->regs[0] != fp_vics[cur])
		return 1;
	*idx = cur;
	return 0;
}

static int tvic_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	int idx = *(int *)ri->data;
	const struct hdmi_timing *t = (void *)regs_return_value(regs);

	if (idx >= 0 && t && t->vic == fp_vics[idx] && t->v_total == fp_base[idx][2])
		regs->regs[0] = (unsigned long)&fp_timing[idx];
	return 0;
}

static struct kretprobe tvic_probe = {
	.kp.symbol_name = "hdmitx21_gettiming_from_vic",
	.entry_handler = tvic_entry,
	.handler = tvic_ret,
	.data_size = sizeof(int),
	.maxactive = 8,
};
static bool tvic_probe_on;

/*
 * the encoder (ENCP) timing of the frame packed modes from hdmitx20's
 * tvregstab_3dfp[] (G12B, the timing CoreELEC's S922X frame packing uses), as ported to hdmitx21 by
 * CoreELEC common_drivers PR #32 ("Use validated fixed ENCP timing for 3D Frame Packing"), written
 * when set_tv_encp_new() returns while frame packed. config_tv_enc_calc()'s doubled 2D timing puts
 * the active lines (DE, VAVON) one line later (42-2246 vs 41-2245), vsync on lines 1-6 (0-5) and DE
 * 2 pixels to the right (196-2116 vs 194-2114). hdmitx21 registers: VPUCTRL (index 0) << 24 | reg
 * << 2.
 */
static void (*p_hd21_write)(u32 vaddr, u32 val);
static u32 (*p_hd21_read)(u32 vaddr);
#define HD21_VPU(r) ((r) << 2)
struct fp_reg {
	u16 reg;
	u16 val;
};
static const struct fp_reg fp_enc_1080p[] = {
	{ 0x1b8d, 0x4040 }, { 0x1b8e, 0x18 },  { 0x1b97, 0xabd },
	{ 0x1bae, 0x8c9 }, /* MODE, ADV, MAX_PX/LNCNT */
	{ 0x1ba4, 0xc0 },   { 0x1ba3, 0x83f }, { 0x1ba6, 0x29 },
	{ 0x1baf, 0x8c5 }, /* HAVON b/e, VAVON b/e */
	{ 0x1ba7, 0x0 },    { 0x1ba8, 0x2c },  { 0x1ba9, 0x1e },
	{ 0x1baa, 0x32 }, /* HSO, VSO begin/end */
	{ 0x1bab, 0x0 },    { 0x1bac, 0x5 }, /* VSO b/e line */
	{ 0x1c30, 0x2 },    { 0x1c31, 0x2e },  { 0x1c32, 0x0 },
	{ 0x1c33, 0x0 }, /* DVI HSO, VSO lines */
	{ 0x1c34, 0x5 },    { 0x1c35, 0x0 },   { 0x1c36, 0x2 },
	{ 0x1c37, 0x0 },    { 0x1c38, 0x2 },   { 0x1c39, 0x0 },
	{ 0x1c3a, 0xc2 },   { 0x1c3b, 0x842 }, { 0x1c3c, 0x29 },
	{ 0x1c3d, 0x8c6 }, /* DE */
	{ 0x1c3e, 0x0 },    { 0x1c3f, 0x0 },
};
static const struct fp_reg fp_enc_720p[] = {
	{ 0x1b8d, 0x4040 }, { 0x1b8e, 0x18 },  { 0x1b97, 0x7bb }, { 0x1bae, 0x5db },
	{ 0x1ba4, 0x104 },  { 0x1ba3, 0x603 }, { 0x1ba6, 0x19 },  { 0x1baf, 0x5d6 },
	{ 0x1ba7, 0x0 },    { 0x1ba8, 0x28 },  { 0x1ba9, 0x1e },  { 0x1baa, 0x32 },
	{ 0x1bab, 0x0 },    { 0x1bac, 0x5 },   { 0x1c30, 0x2 },	  { 0x1c31, 0x2a },
	{ 0x1c32, 0x0 },    { 0x1c33, 0x0 },   { 0x1c34, 0x5 },	  { 0x1c35, 0x0 },
	{ 0x1c36, 0x2 },    { 0x1c37, 0x0 },   { 0x1c38, 0x2 },	  { 0x1c39, 0x0 },
	{ 0x1c3a, 0x106 },  { 0x1c3b, 0x606 }, { 0x1c3c, 0x19 },  { 0x1c3d, 0x5d7 },
	{ 0x1c3e, 0x0 },    { 0x1c3f, 0x0 },
};
#define FP_ENC_N ARRAY_SIZE(fp_enc_1080p)
static u16 fp_enc_calc[FP_ENC_N]; /* what hdmitx21 had computed (the last time it was replaced) */
static u64 fp_enc_sets;

static int encp_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	int *idx = (int *)ri->data;
	int cur = READ_ONCE(fp_cur);

	*idx = -1;
	if (!READ_ONCE(mvc3d_fp_on) || cur < 0 || (u32)regs->regs[2] != fp_vics[cur] ||
	    !p_hd21_write || !p_hd21_read)
		return 1;
	*idx = cur;
	return 0;
}

static int encp_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	int idx = *(int *)ri->data;
	const struct fp_reg *t;
	int i;

	if (idx < 0)
		return 0;
	t = fp_base[idx][1] == 1080 ? fp_enc_1080p : fp_enc_720p;
	/*
	 * only over the frame packed timing hdmitx21 computed (2250/1500 lines): never over
	 * anything else
	 */
	if (p_hd21_read(HD21_VPU(0x1bae)) != (u32)(2 * fp_base[idx][2] - 1)) {
		pr_warn("ENCP lines %u, not the frame packed timing: left as it is\n",
			p_hd21_read(HD21_VPU(0x1bae)));
		return 0;
	}
	for (i = 0; i < FP_ENC_N; i++)
		fp_enc_calc[i] = p_hd21_read(HD21_VPU(t[i].reg));
	for (i = 0; i < FP_ENC_N; i++)
		p_hd21_write(HD21_VPU(t[i].reg), t[i].val);
	fp_enc_sets++;
	pr_info("ENCP frame packed timing (hdmitx20 table): DE h %u-%u v %u-%u (computed %u-%u v %u-%u), vso %u-%u\n",
		p_hd21_read(HD21_VPU(0x1c3a)), p_hd21_read(HD21_VPU(0x1c3b)),
		p_hd21_read(HD21_VPU(0x1c3c)), p_hd21_read(HD21_VPU(0x1c3d)), fp_enc_calc[24],
		fp_enc_calc[25], fp_enc_calc[26], fp_enc_calc[27], p_hd21_read(HD21_VPU(0x1bab)),
		p_hd21_read(HD21_VPU(0x1bac)));
	return 0;
}

static struct kretprobe encp_probe = {
	.kp.symbol_name = "set_tv_encp_new",
	.entry_handler = encp_entry,
	.handler = encp_ret,
	.data_size = sizeof(int),
	.maxactive = 4,
};
static bool encp_probe_on;

/*
 * hdmitx21_set_display (the mode set's hardware set up, from hdmitx_set_current_vmode): the VSIF
 * once more
 */
static int setdisp_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	if (READ_ONCE(mvc3d_fp_on))
		fp_vsif_send();
	return 0;
}

static struct kretprobe setdisp_probe = {
	.kp.symbol_name = "hdmitx21_set_display",
	.handler = setdisp_ret,
	.maxactive = 4,
};
static bool setdisp_probe_on;

struct kread_args {
	const char *path;
	char *buf;
	size_t size;
};

static void kread_now(const char *path, char *buf, size_t size);

static void kread_fn(void *a)
{
	struct kread_args *r = a;

	kread_now(r->path, r->buf, r->size);
}

static void kread(const char *path, char *buf, size_t size)
{
	struct kread_args r = { path, buf, size };
	char mod[32];
	const char *name;

	if (mvc3d_kparam_path(path, mod, sizeof(mod), &name)) {
		mvc3d_kparam(mod, name, NULL, buf, size);
		return;
	}
	mvc3d_in_worker(kread_fn, &r);
}

static void kread_now(const char *path, char *buf, size_t size)
{
	struct file *f = filp_open(path, O_RDONLY, 0);
	loff_t pos = 0;
	ssize_t n = 0;

	buf[0] = 0;
	if (IS_ERR(f))
		return;
	n = kernel_read(f, buf, size - 1, &pos);
	filp_close(f, NULL);
	buf[n > 0 ? n : 0] = 0;
	buf[strcspn(buf, "\n")] = 0;
}

#define HDMITX_SYS "/sys/class/amhdmitx/amhdmitx0/"
#define DV_PARAMS "/sys/module/aml_media/parameters/"

/*
 * after a frame packed mode set: the VSIF checked once more, the VPP's frame packing size and the
 * video window, all in-kernel; after leaving it the mode set itself has cleared the VSIF (and
 * fmtpara_entry too)
 */
static void (*p_set_video_window_ext)(int layer, int *p);
static void fp_work_fn(struct work_struct *w)
{
	if (READ_ONCE(fp_work_on)) {
		int a[4] = { 0, 0, READ_ONCE(fp_win_w) - 1, READ_ONCE(fp_win_h) - 1 };
		char v[12];

		fp_vsif_send();
		snprintf(v, sizeof(v), "%d", READ_ONCE(fp_win_w));
		mvc3d_kparam("aml_media", "framepacking_width", v, NULL, 0);
		snprintf(v, sizeof(v), "%d", READ_ONCE(fp_win_h));
		mvc3d_kparam("aml_media", "framepacking_height", v, NULL, 0);
		snprintf(v, sizeof(v), "%d", READ_ONCE(fp_win_h) - 2 * READ_ONCE(fp_base_h));
		mvc3d_kparam("aml_media", "framepacking_blank", v, NULL, 0);
		if (p_set_video_window_ext)
			p_set_video_window_ext(0, a);
		pr_info("HDMI frame packing on (%s, %dx%d, VSIF len %d, window 0 0 %d %d)\n",
			get_current_vinfo() && get_current_vinfo()->name ?
				get_current_vinfo()->name :
				"?",
			READ_ONCE(fp_win_w), READ_ONCE(fp_win_h), READ_ONCE(fp_vsif_len), a[2],
			a[3]);
	} else {
		pr_info("HDMI frame packing off\n");
	}
}

/* (process context) a client starts / stops frame packed playback */
void mvc3d_fp_arm(bool on)
{
	if (on == mvc3d_fp_armed)
		return;
	if (on) {
		/* a session's counters start here (the module may stay loaded between sessions) */
		fp_vsif_checks = 0;
		fp_vsif_lost = 0;
		fp_hdcp_auths = 0;
		kread(DV_PARAMS "dolby_vision_policy", fp_dv_policy, sizeof(fp_dv_policy));
		kread(DV_PARAMS "dolby_vision_mode", fp_dv_mode, sizeof(fp_dv_mode));
		fp_dv_saved = fp_dv_policy[0] && fp_dv_mode[0];
		if (fp_dv_saved) {
			mvc3d_sysfs_write(DV_PARAMS "dolby_vision_policy", "2");
			mvc3d_sysfs_write(DV_PARAMS "dolby_vision_mode", "5");
		}
		WRITE_ONCE(mvc3d_fp_armed, true);
		pr_info("frame packing armed (Dolby Vision %s/%s -> 2/5)\n", fp_dv_policy,
			fp_dv_mode);
	} else {
		WRITE_ONCE(mvc3d_fp_armed, false);
		if (fp_dv_saved) {
			mvc3d_sysfs_write(DV_PARAMS "dolby_vision_policy", fp_dv_policy);
			mvc3d_sysfs_write(DV_PARAMS "dolby_vision_mode", fp_dv_mode);
		}
		pr_info("frame packing disarmed (the next mode set is 2D)\n");
	}
}

/*
 * the GUI on a 2160p Android mode with 1080p frame packing out: its destination halved (left view
 * lines)
 */
static int plane_check_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct drm_plane_state *state = (struct drm_plane_state *)regs->regs[1];

	if (!READ_ONCE(mvc3d_fp_on) || !state || !state->crtc)
		return 0;
	if (state->crtc_w > READ_ONCE(fp_base_w) || state->crtc_h > READ_ONCE(fp_base_h) ||
	    state->crtc_x >= READ_ONCE(fp_base_w) || state->crtc_y >= READ_ONCE(fp_base_h)) {
		state->crtc_x /= 2;
		state->crtc_y /= 2;
		state->crtc_w = max(state->crtc_w / 2, 1u);
		state->crtc_h = max(state->crtc_h / 2, 1u);
	}
	return 0;
}

static struct kprobe plane_check_probe = {
	.symbol_name = "meson_plane_atomic_check",
	.pre_handler = plane_check_pre,
};
static bool plane_probe_on;

/* the video window while frame packed: the whole frame */
static int win_pre(struct kprobe *p, struct pt_regs *regs)
{
	int *a = (int *)regs->regs[1];

	if (!READ_ONCE(mvc3d_fp_on) || !a)
		return 0;
	a[0] = 0;
	a[1] = 0;
	a[2] = READ_ONCE(fp_win_w) - 1;
	a[3] = READ_ONCE(fp_win_h) - 1;
	return 0;
}

static struct kprobe win_probe = {
	.symbol_name = "_set_video_window",
	.pre_handler = win_pre,
};
static bool win_probe_on;

/*
 * The video layer: the same Dune-era 3D code as the SC2 r25 kernel (5.4 video_sink), so the same
 * five fixes CoreELEC's 5.15 has, here at the PS7718 instructions (found in the PS7718 Image
 * 2026-11-25; the words around each are checked before a probe goes on, else that fix stays off):
 *  - vftype: pre_process_for_3d+0x6c "str w13, [x19, #0xc]" (w13 = VIDTYPE_VIU_FIELD): keeps
 *    VIDTYPE_MVC;
 *  - vsf: vpp_set_filters+0x7a8, after "lsl w10, w8, #1" (FA, not 2D: src_height = 2 *
 *    frame_height): the frame packed MVC picture's own height;
 *  - psel: vd1_set_dcu+0x1048 "and w9, w8, #3" (w8 = vpp_2pic_mode): PSEL 0 (VD1 left, VD2 right);
 *  - vdpos: config_vd_position+0x420 "tst w0, w15" (FA case, w15 = MODE_3D_TO_2D_MASK|OUT_LR): +
 *    MODE_3D_MVC, the whole view;
 *  - vskip: vpp_set_filters_internal+0x604 "cmp w9, w10" (height_in > src_height_max ->
 *    vscale_skip_count++): no line skip for the 2205 line picture.
 */
struct fix_spot {
	const char *sym;
	unsigned int off; /* the probed instruction */
	u32 code[6]; /* the words from off - 0xc */
};

static const struct fix_spot vftype_spot = { "pre_process_for_3d",
					     0x6c,
					     { 0x5282000d, 0x3215016b, 0x5280002a, 0xb9000e6d,
					       0x5280002d, 0x14000003 } };
static const struct fix_spot vsf_spot = { "vpp_set_filters",
					  0x7a8,
					  { 0x17ffff06, 0x378801d8, 0x531f790a, 0x5280004c,
					    0xb900be6c, 0x2a0b03ec } };
static const struct fix_spot psel_spot = { "vd1_set_dcu",
					   0x1048,
					   { 0x52820229, 0x6a09033f, 0x54000440, 0x12000509,
					     0x35000409, 0xf0008588 } };
static const struct fix_spot vdpos_spot = { "config_vd_position",
					    0x420,
					    { 0xb9464e8a, 0x529ec00f, 0x72a0064f, 0x6a0f001f,
					      0x54000600, 0xb9005a6a } };
static const struct fix_spot vskip_spot = { "vpp_set_filters_internal",
					    0x604,
					    { 0x292fbba9, 0xb9406fe9, 0xb81b83a8, 0x6b0a013f,
					      0x54000089, 0xb940aec8 } };

static u64 vsf_fixed, psel_fixed, vftype_fixed, vskip_fixed, vdpos_fixed;

static int vftype_pre(struct kprobe *p, struct pt_regs *regs)
{
	u32 type = *(u32 *)((u8 *)regs->regs[19] + 12);

	if (READ_ONCE(mvc3d_fp_on) && (type & VIDTYPE_MVC)) {
		regs->regs[13] = type;
		vftype_fixed++;
	}
	return 0;
}

static int vsf_pre(struct kprobe *p, struct pt_regs *regs)
{
	/*
	 * only our MVC pictures reach the 3D path while frame packing is on (w24 = process_3d_type)
	 */
	if (READ_ONCE(mvc3d_fp_on) && (regs->regs[24] & 0x21) == 0x21) {
		regs->regs[10] = (u32)regs->regs[10] >> 1;
		vsf_fixed++;
	}
	return 0;
}

static int psel_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (READ_ONCE(mvc3d_fp_on) && (regs->regs[8] & 3)) {
		regs->regs[8] = 0;
		psel_fixed++;
	}
	return 0;
}

static int vdpos_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (READ_ONCE(mvc3d_fp_on) && (regs->regs[0] & 0x821) == 0x821) {
		regs->regs[15] |= 0x800;
		vdpos_fixed++;
	}
	return 0;
}

static int vskip_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (READ_ONCE(mvc3d_fp_on) && (u32)regs->regs[9] > (u32)regs->regs[10] &&
	    (u32)regs->regs[9] <= (u32)READ_ONCE(fp_win_h)) {
		regs->regs[10] = regs->regs[9];
		vskip_fixed++;
	}
	return 0;
}

static struct kprobe vftype_probe = { .pre_handler = vftype_pre };
static struct kprobe vsf_probe = { .pre_handler = vsf_pre };
static struct kprobe psel_probe = { .pre_handler = psel_pre };
static struct kprobe vdpos_probe = { .pre_handler = vdpos_pre };
static struct kprobe vskip_probe = { .pre_handler = vskip_pre };
static bool vftype_probe_on, vsf_probe_on, psel_probe_on, vdpos_probe_on, vskip_probe_on;
static bool vftype_found, vsf_found, psel_found, vdpos_found, vskip_found;

/*
 * (at load) the fix's instruction checked (the 6 words from off - 0xc): its address goes to kp,
 * the probe itself is only registered while a session runs (mvc3d_fp_hooks)
 */
static bool fix_find(struct kprobe *kp, const struct fix_spot *s, const char *what)
{
	struct kprobe k = { .symbol_name = s->sym };
	u32 w[6];
	u8 *a;

	kp->addr = NULL;
	if (register_kprobe(&k)) {
		pr_warn("%s: no %s: fix off\n", what, s->sym);
		return false;
	}
	a = (u8 *)k.addr;
	unregister_kprobe(&k);
	if (probe_kernel_read(w, a + s->off - 0xc, sizeof(w)) || memcmp(w, s->code, sizeof(w))) {
		pr_warn("%s: %s+%#x not the PS7718 code: fix off\n", what, s->sym, s->off);
		return false;
	}
	kp->addr = (kprobe_opcode_t *)(a + s->off);
	return true;
}

/*
 * (Re)registration: register_kprobe() keeps the address it looked up for a probe given by name and
 * refuses a probe with both a name and an address, so the address is cleared again first.
 */
static bool hook_kp(struct kprobe *kp, const char *what)
{
	int r;

	if (kp->symbol_name)
		kp->addr = NULL;
	kp->flags = 0;
	r = register_kprobe(kp);
	if (r)
		pr_warn("%s: probe refused (%d)\n", what, r);
	return !r;
}

static bool hook_krp(struct kretprobe *rp, const char *what)
{
	int r;

	if (rp->kp.symbol_name)
		rp->kp.addr = NULL;
	rp->kp.flags = 0;
	r = register_kretprobe(rp);
	if (r)
		pr_warn("%s: probe refused (%d)\n", what, r);
	return !r;
}

/*
 * amvideo's pre_process_for_3d() takes an MVC picture as the 3D frame alternative one
 * (process_3d_type |= MODE_3D_MVC) only if vf->trans_fmt is set: set here with both eye windows
 * (struct vframe_s: type +12, width +308, height +312, trans_fmt +360, left_eye +376, right_eye
 * +392 - the 5.4.125 layout, the same offsets in the PS7718 pre_process_for_3d, checked by the
 * values first).
 */
#define VF_TYPE_OFF 12
#define VF_WIDTH_OFF 308
#define VF_HEIGHT_OFF 312
#define VF_TRANS_OFF 360
#define VF_LEFT_OFF 376
#define VF_RIGHT_OFF 392
#define TVIN_TFMT_3D_TB_ 5

static u64 fp_conv_done, fp_conv_skip;

void mvc3d_fp_conv_vf(struct vframe_s *vf)
{
	u8 *p = (u8 *)vf;
	u32 *type = (u32 *)(p + VF_TYPE_OFF), w = *(u32 *)(p + VF_WIDTH_OFF),
	    h = *(u32 *)(p + VF_HEIGHT_OFF);
	u32 *trans = (u32 *)(p + VF_TRANS_OFF), *le = (u32 *)(p + VF_LEFT_OFF),
	    *re = (u32 *)(p + VF_RIGHT_OFF);
	static bool logged;

	if (!(*type & VIDTYPE_MVC))
		return;
	if (!logged && READ_ONCE(mvc3d_debug)) {
		logged = true;
		pr_info("MVC picture %ux%u trans_fmt %u left %u %u %u %u right %u %u %u %u\n", w, h,
			*trans, le[0], le[1], le[2], le[3], re[0], re[1], re[2], re[3]);
	}
	if (!((w == 1920 && h == 1080) || (w == 1280 && h == 720)) ||
	    (*trans && *trans != TVIN_TFMT_3D_TB_) || (le[2] && le[2] != w) ||
	    (re[2] && re[2] != w)) {
		fp_conv_skip++;
		return;
	}
	*trans = TVIN_TFMT_3D_TB_;
	le[0] = 0;
	le[1] = 0;
	le[2] = w;
	le[3] = h;
	re[0] = 0;
	re[1] = 0;
	re[2] = w;
	re[3] = h;
	fp_conv_done++;
}

/*
 * The hooks on the HDMI and video paths (mode set, ENCP, VSIF, HDCP, DRM plane, video window and
 * the five instruction fixes) are only registered while someone needs frame packing: a session
 * (MVC3D_HW_START in frame packed mode), an "arm" writer of /dev/mvc3dhw, or fp_test. Loaded and
 * idle, the module is on none of those paths.
 */
static DEFINE_MUTEX(fp_hooks_lock);
static int fp_hooks_users;

/* (fp_hooks_lock held) every hook off, the hdmitx state back */
static void fp_hooks_remove(void)
{
	struct hdmitx_dev *hd = p_get_hdmitx21_device ? p_get_hdmitx21_device() : NULL;

	mvc3d_ui3d_hooks(false, 0, 0);
	atomic_set(&fp_keep_stop, 1);
	cancel_delayed_work_sync(&fp_keep_work);
	if (plane_probe_on)
		unregister_kprobe(&plane_check_probe);
	if (win_probe_on)
		unregister_kprobe(&win_probe);
	if (vsf_probe_on)
		unregister_kprobe(&vsf_probe);
	if (psel_probe_on)
		unregister_kprobe(&psel_probe);
	if (vftype_probe_on)
		unregister_kprobe(&vftype_probe);
	if (vskip_probe_on)
		unregister_kprobe(&vskip_probe);
	if (vdpos_probe_on)
		unregister_kprobe(&vdpos_probe);
	if (hdcp_probe_on)
		unregister_kprobe(&hdcp_probe);
	if (wrrd_probe_on)
		unregister_kretprobe(&wrrd_probe);
	if (encp_probe_on)
		unregister_kretprobe(&encp_probe);
	if (setdisp_probe_on)
		unregister_kretprobe(&setdisp_probe);
	if (tvic_probe_on)
		unregister_kretprobe(&tvic_probe);
	if (fmtpara_probe_on)
		unregister_kretprobe(&fmtpara_probe);
	plane_probe_on = false;
	win_probe_on = false;
	vsf_probe_on = false;
	psel_probe_on = false;
	vftype_probe_on = false;
	vskip_probe_on = false;
	vdpos_probe_on = false;
	hdcp_probe_on = false;
	wrrd_probe_on = false;
	encp_probe_on = false;
	setdisp_probe_on = false;
	tvic_probe_on = false;
	fmtpara_probe_on = false;
	cancel_delayed_work_sync(&fp_work);
	if (hd && (READ_ONCE(mvc3d_fp_on) || fp_attr_forced)) {
		/* (the output stays as it is until the next mode set) */
		fp_flag(hd, false);
		fp_attr(hd, false);
		WRITE_ONCE(mvc3d_fp_on, false);
	}
}

/* (process context) one user more (on) or less (off); 0 or -ENODEV */
int mvc3d_fp_hooks(bool on)
{
	int r = 0;

	if (!mvc3d_fp_ok)
		return on ? -ENODEV : 0;
	mutex_lock(&fp_hooks_lock);
	if (on) {
		if (fp_hooks_users++ == 0) {
			fmtpara_probe_on = hook_krp(&fmtpara_probe, "mode set (fmtpara)");
			tvic_probe_on = fmtpara_probe_on && hook_krp(&tvic_probe, "mode set (timing)");
			if (!tvic_probe_on) {
				fp_hooks_remove();
				fp_hooks_users = 0;
				pr_warn("no mode set probes: no frame packing\n");
				r = -ENODEV;
				goto out;
			}
			setdisp_probe_on = hook_krp(&setdisp_probe, "set_display");
			encp_probe_on = p_hd21_write && p_hd21_read && hook_krp(&encp_probe, "ENCP");
			wrrd_probe_on = wrrd_probe.kp.addr && hook_krp(&wrrd_probe, "VSIF re-assert");
			hdcp_probe_on = hook_kp(&hdcp_probe, "HDCP");
			plane_probe_on = hook_kp(&plane_check_probe, "DRM plane");
			win_probe_on = hook_kp(&win_probe, "video window");
			vftype_probe_on = vftype_found && hook_kp(&vftype_probe, "vftype");
			vsf_probe_on = vsf_found && hook_kp(&vsf_probe, "vsf");
			psel_probe_on = psel_found && hook_kp(&psel_probe, "psel");
			vdpos_probe_on = vdpos_found && hook_kp(&vdpos_probe, "vdpos");
			vskip_probe_on = vskip_found && hook_kp(&vskip_probe, "vskip");
			mvc3d_ui3d_hooks(true, READ_ONCE(fp_base_h), READ_ONCE(fp_win_h));
			atomic_set(&fp_keep_stop, 0);
			mod_delayed_work(system_wq, &fp_keep_work, msecs_to_jiffies(1000));
			pr_info("frame packing hooks on (ENCP %d re-assert %d set_display %d; fixes vftype %d vsf %d psel %d vdpos %d vskip %d; UI over both eyes possible %d)\n",
				encp_probe_on, wrrd_probe_on, setdisp_probe_on, vftype_probe_on,
				vsf_probe_on, psel_probe_on, vdpos_probe_on, vskip_probe_on,
				mvc3d_ui3d_ready());
		}
	} else if (fp_hooks_users > 0 && --fp_hooks_users == 0) {
		fp_hooks_remove();
		pr_info("frame packing hooks off\n");
	}
out:
	mutex_unlock(&fp_hooks_lock);
	return r;
}

/*
 * tests: fp_test=1 arms frame packing without a player (hooks on; the next 1080p24/25/30 mode set
 * is frame packed); set a 2D mode (e.g. 1080p60hz) first, then fp_test=0 (disarmed, hooks off)
 */
static int fp_test;
static bool fp_test_ref;
static void fp_test_fn(struct work_struct *w)
{
	bool on = READ_ONCE(fp_test) != 0;

	if (on && !fp_test_ref) {
		if (mvc3d_fp_hooks(true))
			return;
		fp_test_ref = true;
		mvc3d_fp_arm(true);
	} else if (!on && fp_test_ref) {
		mvc3d_fp_arm(false);
		fp_test_ref = false;
		mvc3d_fp_hooks(false);
	}
}
static DECLARE_WORK(fp_test_work, fp_test_fn);

static int fp_test_set(const char *val, const struct kernel_param *kp)
{
	int r = param_set_int(val, kp);

	if (!r && mvc3d_fp_ok)
		schedule_work(&fp_test_work);
	return r;
}
static const struct kernel_param_ops fp_test_ops = { .set = fp_test_set, .get = param_get_int };
module_param_cb(fp_test, &fp_test_ops, &fp_test, 0644);
MODULE_PARM_DESC(
	fp_test,
	"tests: 1 = frame packing armed and hooked without a player, 0 = off (set a 2D mode first)");

/* (at load) everything checked and looked up; no hook is registered here (mvc3d_fp_hooks) */
void mvc3d_fp_init(void)
{
	const struct hdmi_timing *(*tvic)(enum hdmi_vic vic);
	struct hdmitx_dev *hd;
	char attr[17];
	int i;

	tvic = (void *)kallsyms_lookup_name("hdmitx21_gettiming_from_vic");
	p_get_hdmitx21_device = (void *)kallsyms_lookup_name("get_hdmitx21_device");
	p_get_current_vinfo = (void *)kallsyms_lookup_name("get_current_vinfo");
	if (!p_get_hdmitx21_device || !p_get_current_vinfo)
		return;
	hd = p_get_hdmitx21_device();
	if (!hd || !tvic) {
		pr_warn("no hdmitx21: no frame packing\n");
		return;
	}
	/*
	 * the struct offsets, checked by their values: fmt_attr a "...bit" string, para a kernel
	 * pointer, no 3D flag
	 */
	memcpy(attr, hd->fmt_attr, 16);
	attr[16] = 0;
	if (hd->flag_3dfp || hd->flag_3dtb || hd->flag_3dss || !strstr(attr, "bit") ||
	    ((unsigned long)hd->para >> 48) != 0xffff) {
		pr_warn("hdmitx21 layout not the PS7718 one (flags %d%d%d, attr \"%s\"): no frame packing\n",
			hd->flag_3dfp, hd->flag_3dtb, hd->flag_3dss, attr);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(fp_vics); i++)
		if (!fp_make_timing(i, tvic(fp_vics[i]))) {
			pr_warn("hdmitx21 VIC %d timing not the CEA one: no frame packing\n",
				fp_vics[i]);
			return;
		}
	p_vend_rawset = (void *)kallsyms_lookup_name("hdmi_vend_infoframe_rawset");
	p_set_video_window_ext = (void *)kallsyms_lookup_name("set_video_window_ext");
	if (!p_vend_rawset) {
		pr_warn("no hdmi_vend_infoframe_rawset: no frame packing\n");
		return;
	}
	INIT_DELAYED_WORK(&fp_work, fp_work_fn);
	p_infoframe_send = (void *)kallsyms_lookup_name("hdmitx_infoframe_send");
	p_hd21_write = (void *)kallsyms_lookup_name("hd21_write_reg");
	p_hd21_read = (void *)kallsyms_lookup_name("hd21_read_reg");
	wrrd_probe.kp.addr = (kprobe_opcode_t *)kallsyms_lookup_name(
		"_tpi_infoframe_wrrd.llvm.6229976626646311496");
	p_infoframe_rawget = (void *)kallsyms_lookup_name("hdmitx_infoframe_rawget");
	INIT_DELAYED_WORK(&fp_keep_work, fp_keep_fn);
	atomic_set(&fp_keep_stop, 1);
	fp_keep_ok = true;
	p_is_amdv_on = (void *)kallsyms_lookup_name("is_amdv_on");
	p_is_amdv_enable = (void *)kallsyms_lookup_name("is_amdv_enable");
	vftype_found = fix_find(&vftype_probe, &vftype_spot, "vftype");
	vsf_found = fix_find(&vsf_probe, &vsf_spot, "vsf");
	psel_found = fix_find(&psel_probe, &psel_spot, "psel");
	vdpos_found = fix_find(&vdpos_probe, &vdpos_spot, "vdpos");
	vskip_found = fix_find(&vskip_probe, &vskip_spot, "vskip");
	mvc3d_fp_ok = true;
	pr_info("HDMI frame packing ready (hdmitx21, attr \"%s\"; infoframe_send %d window %d; fixes found vftype %d vsf %d psel %d vdpos %d vskip %d; hooks per session)\n",
		attr, !!p_infoframe_send, !!p_set_video_window_ext, vftype_found, vsf_found,
		psel_found, vdpos_found, vskip_found);
}

void mvc3d_fp_exit(void)
{
	if (mvc3d_fp_ok)
		cancel_work_sync(&fp_test_work);
	if (mvc3d_fp_armed)
		mvc3d_fp_arm(false);
	mutex_lock(&fp_hooks_lock);
	if (fp_hooks_users) {
		fp_hooks_remove();
		fp_hooks_users = 0;
	}
	mutex_unlock(&fp_hooks_lock);
}

/*
 * (process context or a worker) the VPP's 3D type, as AMSTREAM_IOC_SET_3D_TYPE on /dev/amvideo does
 * it for a player (CoreELEC's Kodi: MODE_3D_ENABLE | MODE_3D_FA with framepacking_support for MVC
 * frame packing): VD1 reads the left eye, VD2 the right eye, lines 0-1079 and 1125-2204 of the 2205
 * line frame. 0: off. Through amvideo's own threedim_mode attribute, whose store runs the same code
 * as that ioctl: SELinux does not let the kernel open /dev/amvideo (-13), and players need not.
 */
#define SYS_3D_TYPE "class/video/threedim_mode"

int mvc3d_fp_3d_type(u32 type)
{
	char v[16];
	int r;

	snprintf(v, sizeof(v), "%#x", type);
	r = mvc3d_sys_write(SYS_3D_TYPE, v);
	pr_info("3D type %#x: %d\n", type, r);
	return r;
}

/* amvideo's 3D type now ("process type 0x%x,trans fmt %u."), or -1 */
static int fp_3d_type_get(void)
{
	char v[64];
	unsigned int t;

	if (mvc3d_sys_read(SYS_3D_TYPE, v, sizeof(v)) || sscanf(v, "process type %x", &t) != 1)
		return -1;
	return t;
}

/*
 * (the keeper, while a frame packed session shows pictures) a decoder restart can set the 3D type
 * back: set again, as the root player used to every 2 s
 */
static u64 fp_3d_reasserts;
static void fp_3d_type_keep(void)
{
	int t = fp_3d_type_get();

	if (t < 0 || (t & 0xff) == (MODE_3D_ENABLE | MODE_3D_FA))
		return;
	fp_3d_reasserts++;
	if (fp_3d_reasserts <= 20)
		pr_info("3D type was %#x: set again\n", t);
	mvc3d_fp_3d_type(MODE_3D_ENABLE | MODE_3D_FA);
}

void mvc3d_fp_session_start(void)
{
	colour_logged = 0;
	fp_3d_reasserts = 0;
}

void mvc3d_fp_summary(void)
{
	if (mvc3d_fp_ok)
		pr_info("VSIF checks %llu lost %llu, HDCP auths %llu, 3D type set again %llu, UI frames over both eyes %llu\n",
			fp_vsif_checks, fp_vsif_lost, fp_hdcp_auths, fp_3d_reasserts,
			mvc3d_ui3d_stretched());
}

void mvc3d_fp_stats(struct seq_file *m)
{
	seq_printf(
		m,
		"frame packing %s, armed %d, on %d, mode sets %llu, vinfo maps %llu, VPP height fixes %llu, PSEL fixes %llu, type fixes %llu, vskip fixes %llu, position fixes %llu, MVC pictures marked %llu (skipped %llu)\n",
		mvc3d_fp_ok ? "ready" : "OFF", mvc3d_fp_armed, mvc3d_fp_on, fp_modesets, fp_maps,
		vsf_fixed, psel_fixed, vftype_fixed, vskip_fixed, vdpos_fixed, fp_conv_done,
		fp_conv_skip);
	if (p_hd21_read)
		seq_printf(
			m,
			"ENCP: table sets %llu, lines %u px %u, DE h %u-%u v %u-%u, VAVON %u-%u, vso %u-%u, dvi vso %u-%u\n",
			fp_enc_sets, p_hd21_read(HD21_VPU(0x1bae)) + 1,
			p_hd21_read(HD21_VPU(0x1b97)) + 1, p_hd21_read(HD21_VPU(0x1c3a)),
			p_hd21_read(HD21_VPU(0x1c3b)), p_hd21_read(HD21_VPU(0x1c3c)),
			p_hd21_read(HD21_VPU(0x1c3d)), p_hd21_read(HD21_VPU(0x1ba6)),
			p_hd21_read(HD21_VPU(0x1baf)), p_hd21_read(HD21_VPU(0x1bab)),
			p_hd21_read(HD21_VPU(0x1bac)), p_hd21_read(HD21_VPU(0x1c32)),
			p_hd21_read(HD21_VPU(0x1c34)));
	seq_printf(
		m,
		"frame packing VSIF: sent %llu, re-asserted %llu, cleared %llu (re-assert probe %d, set_display probe %d), window %dx%d\n",
		fp_vsif_sent, fp_vsif_reasserts, fp_vsif_clears, wrrd_probe_on, setdisp_probe_on,
		fp_win_w, fp_win_h);
	seq_printf(m,
		   "VSIF keeper: checks %llu, lost %llu, HDCP auths %llu (probe %d, rawget %d)\n",
		   fp_vsif_checks, fp_vsif_lost, fp_hdcp_auths, hdcp_probe_on,
		   !!p_infoframe_rawget);
}
