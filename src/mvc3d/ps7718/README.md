# mvc3d kernel helper (Fire TV Cube 3rd gen, PS7718)

Out-of-tree kernel module that plays Blu-ray 3D (H.264 MVC) through the Cube's hardware MVC decoder
(`amvdec_h264mvc`) and the legacy video layer, with HDMI 1.4 frame packing (1080p24/25/30, 720p60/50). A port
of djnice's S905X4 module (vlc-fel-android `kernel/mvc3d/sc2`).

`video/mvc3d-service.sh` loads it at boot (`ENABLE_MVC3D=1`); it stays loaded and idle - no probes on any HDMI,
video or decoder path - until a player starts a session on `/dev/mvc3dhw`, and is idle again when that
session's file is closed. See `docs/MVC3D-INTERFACE.md`. 2D, HDR, Dolby Vision and FEL playback never run
through it outside a 3D session.

Target: Fire TV Cube 3rd gen "gazelle" (AFTGAZL, Amlogic T7), Fire OS 7.7.1.8 (PS7718.5826N), kernel
5.4.125-ab35. Every PS7718 symbol, struct offset and instruction it depends on is listed in the table at the
top of `mvc3d.h` and checked at load; on a mismatch the feature concerned stays off.

| file | contents |
|------|----------|
| `mvc3d_main.c` | `/dev/mvc3dhw`, vfm receiver/provider between the decoder and amvideo, picture timing, ordered put-back, sessions, module parameters, debugfs |
| `mvc3d_fp.c` | HDMI frame packing on hdmitx21: mode set/timing probes, 3D VSIF and its keeper, video layer fixes; registered per session |
| `mvc3d_sys.c` | a frame packed session's display (HDMI mode and attribute, UI planes, video layer) set up and put back |
| `mvc3d_pool.c` | the MVC decoder's work space and reference buffers kept below 2 GiB (its 31 bit DMA); probes per session |
| `mvc3d.h` | shared declarations and the PS7718 dependency table |
| `mvc3d_uapi.h` | the `/dev/mvc3dhw` ioctl interface (same ABI as the S905X4 module in vlc-fel-android) |

## Parameters

| name | default | |
|------|---------|--|
| `fp_enable` | 0 | HDMI frame packing (`mvc3d-service.sh` loads with 1); checked at load, hooked per session |
| `own_display` | 1 | a frame packed session's display is set up and put back by the helper (0: the player does it) |
| `fp_vsif_len` | 5 | 3D VSIF length (5 = HDMI 1.4b PB1-PB5) |
| `fp_test` | 0 | tests: 1 = frame packing hooked and armed without a player; set a 2D mode, then 0 |
| `put_delay_vsync` | 1 | pictures go back to the decoder 2 + this vsyncs after the display released them (1 fixes tearing on heavy scene cuts) |
| `debug` | 0 | diagnostics in the kernel log (buffer/scan-out checks, colour state, session summary) and debugfs `vsynclog` |

## Build

`build-video.sh` builds it with the FEL helpers, against the prepared PS7718 kbuild (`gazelle-fel-7718`).

It was developed against Amazon's FireTVCubeGen3 7.7.1.4 GPL source (`kernel/amlogic/pop1g/5.4/common` and its
`prebuilts/clang/host/linux-x86/clang-r377782b`), configured from the device's `/proc/config.gz` (identical option
for option), with a `Module.symvers` for PS7718 generated from the PS7718 `boot.img` kernel
(`__ksymtab`/`__kcrctab`, cross-checked against the vendor modules' `__versions`):

```
make -C <kernel>/common O=<kout> ARCH=arm64 CC=clang LD=ld.lld CLANG_TRIPLE=aarch64-linux-gnu- \
     CROSS_COMPILE=aarch64-linux-gnu- CONFIG_AMLOGIC_LTO_CLANG= CONFIG_LTO= LOCALVERSION=-ab35 \
     M=$PWD KBUILD_EXTRA_SYMBOLS=<path>/Module.symvers.ps7718 modules
```

The .14 helper built that way is instruction for instruction the same as the one built from the 7.7.1.3 source
that ran on the Cube (88 functions, 7849 instructions; only build path strings differ).
