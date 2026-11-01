# FEL player interface

The module exposes `/dev/dvfel` to Android apps after its firmware guards and
helper configuration succeed. No package allowlist, UID provisioning or player
root grant is required. Installing a player later does not require a reboot.
The initial Magisk module installation still requires a reboot.

Opening the device or querying `INFO` does not activate FEL. A compatible player
must explicitly `START`, decode the enhancement layer, queue its native UVM
dma-bufs, and supply base-layer timestamp/RPU hints. This does not add FEL support
to an unmodified player. The existing driver hooks remain loaded but session
processing is inactive without an owner (apart from outstanding-frame teardown).

The UAPI is in `src/fel/ps7702/dvfel_sc2_uapi.h`; PS7717 has the same ABI.
[libdvfelplay](https://github.com/signde/vlc-fel-android/tree/main/libdvfelplay)
provides the existing player integration. Its scoped-node lookup falls back to
`/dev/dvfel`, so existing Kodi FEL and VLC FEL builds continue to work.

- `INFO`: query version without acquiring a session.
- `START`: claim the session for this open file description. A competing owner
  or an unfinished drain returns `EBUSY`.
- `BL_HINT`: submit BL PTS and RPU CRC; reserved fields must be zero.
- `EL_QUEUE`: submit a decoder-backed UVM dma-buf FD, not an address. Reserved
  fields must be zero; duplicate live buffers and full queues are rejected.
  The returned sync-file fence protects decoder-buffer lifetime.
- `WAIT_DONE`: read this session’s completed buffer IDs, with a timeout of
  0–1000 ms. It cannot drain another session’s completions. A stop/restart
  invalidates an outstanding wait.
- `FLUSH`: accept only `FLUSH_EL` / `FLUSH_HINTS`; displayed buffers remain pinned.
- `STOP` or last close: stop pairing and drain before another session starts.

The helper bounds EL frames and BL hints, validates ioctl sizes and user copies,
and serializes submission against session teardown. Access is tied to the open
file description, so passing that descriptor also passes session authority.

Access is intentionally available to all apps through mode 0666 and a rule for
this device type only. An app can deliberately claim a session and disrupt
playback; this is not a security boundary against hostile apps. No global
SELinux permissive mode is used. These checks are not a full security audit of
Amlogic’s decoder/UVM drivers.

`tests/fel-ioctl.c` exercises invalid commands, ownership, waits and cleanup on
an idle Cube. Do not run it during media playback.
