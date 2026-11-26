# MVC 3D player interface

The module loads the MVC 3D helper (`mvc3d.ko`, PS7718.5826N so far) at boot after its firmware guards, and
exposes `/dev/mvc3dhw` to Android apps the same way as `/dev/dvfel` (its own SELinux type, mode 0666). No
package allowlist or player root grant is needed for the device. `ENABLE_MVC3D=0` in `config.sh` (and a reboot)
leaves the helper out.

Loaded and idle, the helper is on no HDMI, video or decoder path: no probes are registered. Opening the device
or reading its status does not start anything. A session belongs to the open file description that started it:

- `MVC3D_HW_START` (`struct mvc3d_hw_start`: one view's `width` x `height`, 1920x1080 or 1280x720, `mode`
  `MVC3D_HW_FP` or `MVC3D_HW_2D`): claims the helper (`EBUSY` if another file owns it or another player's
  decoder is still open). It registers the session's probes (the MVC decoder's allocations; for frame packing
  the HDMI mode set, ENCP, VSIF, HDCP, DRM plane, video window and five instruction fixes), saves the display
  (HDMI mode, colour attribute, UI planes, video layer, `framepacking_support`, Dolby Vision settings),
  switches to 1080p24 frame packed and waits until frame packing is on (`EIO` and everything put back if it
  does not come on), hides the UI planes and routes `decoder.h264mvc` through the helper to amvideo. It takes
  3-6 s. Returns `gen` and `reserved[0] = MVC3D_HW_F_DIRECT`.
- The player feeds the stock hardware MVC decoder (`amvdec_h264mvc`) itself - libmvc3dplay's hardware path
  (djnice's [vlc-fel-android](https://github.com/djnice/vlc-fel-android), `libmvc3dplay/src/mvc3dhw.c`) - and
  then, per decoded picture: `MVC3D_HW_WAIT` (the next picture, with its `gen`), `MVC3D_HW_PRESENT` (show it
  from the vsync nearest `due_ns`, `CLOCK_MONOTONIC`) or `MVC3D_HW_RELEASE` (drop it). `MVC3D_HW_FLUSH` drops
  everything queued (a seek) and returns the new `gen`.
- The last close of the file (also when the player crashes): the display put back as it was before, the route
  back to default, the session's probes removed - idle again.

`read()` returns one status line, `armed=N on=N started=N`. Writing `arm` arms frame packing (and registers the
HDMI probes) until that file is closed, for players that set the HDMI mode themselves.

The UAPI is in `src/mvc3d/ps7718/mvc3d_uapi.h` (the same ABI as djnice's S905X4 module).

Feeding the decoder needs `/dev/amstream_vbuf` and `/dev/amvideo`, which apps cannot open by default; today's
Wholphin FEL build runs that part as a root helper (`mvchwd`, through su) for its 3D sessions. A rule giving
apps those two nodes as well would let players do it in-process; it is not part of this module yet.

Access is intentionally available to all apps, as for `/dev/dvfel`: an app can claim a session and switch the
HDMI output; this is not a security boundary against hostile apps. No global SELinux permissive mode is used.

Not covered by an automated test. The frame packing itself (helper versions up to .15, loaded per session by
Wholphin FEL's root script) was hardware-tested on a Fire TV Cube 3rd gen (PS7718.5826N) with a 3D projector:
Avatar, Billy Lynn's Long Halftime Walk, Money From Home, Dune, Dredd, Mission: Impossible - Fallout. This
shared form (.16: loaded at boot, idle until a session, the display set up in-kernel) is not hardware-tested
yet.
