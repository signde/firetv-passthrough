/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
/*
 * mvc3d userspace interface: /dev/mvc3dhw, hardware MVC (Blu-ray 3D) playback.
 *
 * The player feeds the stream (frame_num rewritten, see libmvc3dplay mvcfnr) to the legacy Amlogic
 * MVC decoder (/dev/amstream_vbuf, VFORMAT_H264MVC). While a client has MVC3D_HW_START'ed (the
 * module answers reserved[0] = MVC3D_HW_F_DIRECT), the decoder's own pictures go to the display
 * through this module (vfm "decoder.h264mvc mvc3dhw amvideo", the legacy video layer: the VPP shows
 * the base view, or with frame packing both views itself). There are no client buffers.
 *
 * MVC3D_HW_WAIT gives a picture slot (index), in display order; the client passes it on with
 * MVC3D_HW_PRESENT at its display time (the slot is shown from the first vsync near due_ns and goes
 * back to the decoder by itself) or drops it with MVC3D_HW_RELEASE. The decoder's first two
 * pictures after a start are not valid (seq 0 and 1: drop them); its last pictures stay in it at
 * the end of the stream. MVC3D_HW_FLUSH (seek) drops the pictures not yet taken.
 *
 * The ioctl numbers and struct layouts are those of the S905X4 (SC2) module in vlc-fel-android, so
 * libmvc3dplay works unchanged.
 */
#ifndef _UAPI_MVC3D_H
#define _UAPI_MVC3D_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MVC3D_IOC_MAGIC		'3'

#define MVC3D_HW_DEV		"/dev/mvc3dhw"
#define MVC3D_HW_MAX_BUFS	12
#define MVC3D_HW_2D		0
#define MVC3D_HW_FP		1

/* MVC3D_HW_START out reserved[0]: the decoder's pictures straight to the display (the only mode) */
#define MVC3D_HW_F_DIRECT	1
/*
 * MVC3D_HW_START reserved[1]: the Android UI over both eyes of a frame packed 1080p session (the 1920x1080
 * UI stretched to the 1920x2205 frame: rows 0-529 show in the left eye, rows 551-1080 in the right one).
 * in: the player asks for it (it draws its UI once per eye); a player that leaves reserved[1] 0 gets the
 * UI hidden for the session, as before. out: granted (else 0: hidden).
 */
#define MVC3D_HW_F_UI3D		1

struct mvc3d_hw_start {
	__u32 width;		/* in: one view (1920x1080 or 1280x720) */
	__u32 height;
	__u32 mode;		/* in: MVC3D_HW_2D / MVC3D_HW_FP */
	__u32 out_height;	/* out: 2D: height; FP: 2 * height + active space */
	__u32 stride;		/* out: unused in direct mode */
	__u32 gen;		/* out: pictures of this start have this gen (FLUSH changes it) */
	/*
	 * in: reserved[1] = MVC3D_HW_F_UI3D asked or 0;
	 * out: reserved[0] = MVC3D_HW_F_DIRECT, reserved[1] = MVC3D_HW_F_UI3D granted or 0
	 */
	__u32 reserved[2];
};

struct mvc3d_hw_buf {
	__s32 fd;		/* unused in direct mode */
	__s32 index;		/* RELEASE in: the slot */
};

struct mvc3d_hw_pic {
	__u32 timeout_ms;	/* in */
	__s32 index;		/* out: the picture slot (client owns it now) */
	__u32 seq;		/* out: decoder picture number since the decoder started */
	__u32 gen;		/* out: MVC3D_HW_FLUSH count */
	__u64 decoded_ns;	/* out: CLOCK_MONOTONIC when got from the decoder */
	__u32 reserved[2];
};

struct mvc3d_hw_present {
	__s32 index;		/* in: a slot from MVC3D_HW_WAIT */
	__u32 reserved;
	__u64 due_ns;		/* in: CLOCK_MONOTONIC display time */
};

#define MVC3D_HW_START		_IOWR(MVC3D_IOC_MAGIC, 0x20, struct mvc3d_hw_start)
/* not supported in direct mode (-EOPNOTSUPP) */
#define MVC3D_HW_ADD_BUF	_IOWR(MVC3D_IOC_MAGIC, 0x21, struct mvc3d_hw_buf)
#define MVC3D_HW_WAIT		_IOWR(MVC3D_IOC_MAGIC, 0x22, struct mvc3d_hw_pic)
/* the slot dropped by the client */
#define MVC3D_HW_RELEASE	_IOW(MVC3D_IOC_MAGIC, 0x23, struct mvc3d_hw_buf)
/* seek: pictures not yet taken are dropped; out: the gen of the pictures from now on */
#define MVC3D_HW_FLUSH		_IOR(MVC3D_IOC_MAGIC, 0x24, __u32)
/* __u32 1: -ENODEV if HDMI frame packing is not available (MVC3D_HW_START in FP mode arms it) */
#define MVC3D_HW_SET_FP		_IOW(MVC3D_IOC_MAGIC, 0x25, __u32)
#define MVC3D_HW_PRESENT	_IOW(MVC3D_IOC_MAGIC, 0x26, struct mvc3d_hw_present)

#endif /* _UAPI_MVC3D_H */
