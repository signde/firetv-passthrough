# mvc3d kernel module (Fire TV Cube 3rd gen)

Out-of-tree kernel module that plays Blu-ray 3D (H.264 MVC) through the Cube's hardware MVC decoder
(`amvdec_h264mvc`) and the legacy video layer, with HDMI 1.4 frame packing (1080p24/25/30, 720p60/50).
It is loaded per playback session by `app/src/main/assets/mvc3d/mvc3d_session.sh` and removed afterwards.
2D, Dolby Vision and FEL playback never touch it.

Target: Fire TV Cube 3rd gen "gazelle" (AFTGAZL, Amlogic T7), Fire OS 7.7.1.8 (PS7718), kernel
5.4.125-ab35, rooted (the session runs through `su`). Every PS7718 symbol, struct offset and instruction it
depends on is listed in the table at the top of `mvc3d.h`; on a mismatch the feature concerned stays off.

| file | contents |
|------|----------|
| `mvc3d_main.c` | `/dev/mvc3dhw`, vfm receiver/provider between the decoder and amvideo, picture timing, ordered put-back, module parameters, debugfs |
| `mvc3d_fp.c` | HDMI frame packing on hdmitx21: mode set/timing probes, 3D VSIF and its keeper, video layer fixes |
| `mvc3d_pool.c` | the MVC decoder's work space and reference buffers kept below 2 GiB (its 31 bit DMA) |
| `mvc3d.h` | shared declarations and the PS7718 dependency table |
| `mvc3d_uapi.h` | the `/dev/mvc3dhw` ioctl interface (same ABI as the S905X4 module in vlc-fel-android) |

## Parameters

| name | default | |
|------|---------|--|
| `fp_enable` | 0 | HDMI frame packing probes (the session script loads with 1) |
| `fp_vsif_len` | 5 | 3D VSIF length (5 = HDMI 1.4b PB1-PB5) |
| `fp_test` | 0 | 1 = frame packing armed without a player (the session script uses it for the mode set) |
| `put_delay_vsync` | 1 | pictures go back to the decoder 2 + this vsyncs after the display released them (1 fixes tearing on heavy scene cuts) |
| `debug` | 0 | diagnostics in the kernel log (buffer/scan-out checks, colour state, session summary) and debugfs `vsynclog`; the session script loads with `debug=1` when `/data/local/tmp/wholphin3d/debug` exists and keeps the log in `last_session_dmesg.txt` |

## Build

Kernel tree: Amazon's FireTVCubeGen3 7.7.1.3 GPL source (`kernel/amlogic/pop1g/5.4/common`) configured with the
device's `/proc/config.gz`, prebuilt clang r377782b. `Module.symvers` for PS7718 is not published; it was
generated from the PS7718 `boot.img` kernel (`__ksymtab`/`__kcrctab`, cross-checked against the vendor modules'
`__versions`).

```
make -C <kernel>/common O=<kout> ARCH=arm64 CC=clang LD=ld.lld CLANG_TRIPLE=aarch64-linux-gnu- \
     CROSS_COMPILE=aarch64-linux-gnu- CONFIG_LTO= LOCALVERSION=-ab35 \
     M=$PWD KBUILD_EXTRA_SYMBOLS=<path>/Module.symvers.ps7718 modules
```

Strip the debug info (`<clang>/bin/llvm-strip --strip-debug mvc3d.ko`), then copy it to
`app/src/main/assets/mvc3d/mvc3d.ko`; `VER` in `mvc3d_session.sh` must match the module's
`MVC3D_MODULE_VERSION`.

checkpatch (v5.4 `scripts/checkpatch.pl --no-tree --file --max-line-length=100`): 0 errors, 0 warnings.
