#!/system/bin/sh
# MVC 3D (Blu-ray 3D) frame packing helper: loaded once, idle (on no HDMI, video or decoder path)
# until a player starts a session on /dev/mvc3dhw; the session's hooks and display are undone when it
# stops or its file is closed.
M=$1
OWNER=${M%/*}
umask 077
exec 9>/dev/firetv_mvc3d_start.lock
/data/adb/magisk/busybox flock -n 9 || exit 0
status() { echo "$*" > "$M/mvc3d-status.txt"; echo "$*"; }
fail() { [ ! -e /dev/mvc3dhw ] || chmod 000 /dev/mvc3dhw 2>/dev/null; status "$*"; exit 1; }
stop_requested() { [ -e "$OWNER/disable" ] || [ -e "$OWNER/remove" ]; }
status WAITING
# Do not block Android boot; the stock MVC decoder must be there before the helper.
i=0
while [ "$i" -lt 120 ]; do
 stop_requested && fail DISABLED
 if [ "$(getprop sys.boot_completed)" = 1 ] && grep -q '^amvdec_h264mvc ' /proc/modules; then break; fi
 sleep 2; i=$((i+1))
done
[ "$i" -lt 120 ] || fail 'WAITING_FAILED: stock MVC decoder unavailable'
. "$M/verify-device.sh"
verify_video_device || fail UNSUPPORTED
KO="$M/mvc3d/$FEL_PROFILE/mvc3d.ko"
[ -f "$KO" ] || fail "UNSUPPORTED_BUILD: no MVC 3D helper for $FEL_PROFILE"
if [ -d /sys/module/mvc3d ]; then
 fail 'ALREADY_LOADED: reboot to replace the MVC 3D helper'
fi
# Kernel symbol, layout and instruction guards are checked by the helper at load; nothing is hooked
# until a session starts.
insmod "$KO" fp_enable=1 own_display=1 || fail 'LOAD_FAILED: inspect kernel log; no forced load attempted'
# Android creates the device node asynchronously after misc_register returns.
i=0
while [ ! -c /dev/mvc3dhw ] && [ "$i" -lt 50 ]; do
 sleep 0.1; i=$((i+1))
done
[ -c /dev/mvc3dhw ] || fail DEVICE_NODE_TIMEOUT
grep -q 'frame packing ready' /sys/kernel/debug/mvc3d/hw_stats 2>/dev/null ||
 fail 'FRAME_PACKING_UNAVAILABLE: inspect kernel log'
. "$OWNER/scripts/mvc3d-access.sh"
provision_mvc3d_access || fail PLAYER_ACCESS_FAILED
# players feed the decoder themselves (unless ENABLE_MVC3D_APP_DECODER=0 in config.sh)
[ -f "$OWNER/config.sh" ] && . "$OWNER/config.sh"
if [ "$ENABLE_MVC3D_APP_DECODER" != 0 ]; then
 provision_mvc3d_decoder_access || fail DECODER_ACCESS_FAILED
fi
status "READY: $(cat /sys/module/mvc3d/version), shared MVC 3D interface"
# No rmmod on shutdown, disable or app exit; a reboot removes the helper.
