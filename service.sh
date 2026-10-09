#!/system/bin/sh
# Magisk runs this in BusyBox ash standalone mode. Workers block on events.
MODDIR=${0%/*}
umask 077
exec 8>/dev/firetv_passthrough.lock
flock -n 8 || exit 0
[ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ] || exit 0
. "$MODDIR/verify-firmware.sh"
. "$MODDIR/scripts/legacy.sh"
fail() { echo "$*" > "$MODDIR/status.txt"; exit 1; }
legacy_enabled /data/adb && fail "LEGACY_MODULE_ENABLED: disable the old modules and reboot"
verify_firmware "$MODDIR" || fail "UNSUPPORTED"
(cd "$MODDIR" && sha256sum -c payload.sha256 >/dev/null 2>&1) || fail "CORRUPT_PAYLOAD"
[ -x /system/bin/aparam ] || fail "APARAM_UNAVAILABLE"
if [ "$(getprop ro.product.device)" = karat ]; then
    actual=$(sha256sum /system/bin/aparam)
    [ "${actual%% *}" = 690cfd8c33e3c02d68c7e0d1c51415530907bcf41f1cf8784186ba17c897e4f2 ] ||
        fail "KARAT_OVERLAY_UNAVAILABLE: reboot after installation"
fi
# Separate subshells keep the workers' traps, logs and legacy-compatible locks
# independent. Retain the old DTS attachment marker to prevent duplicate hooks.
( . "$MODDIR/scripts/dolby-service.sh" ) 8>&- &
DOLBY_PID=$!
( . "$MODDIR/scripts/dts-service.sh" ) 8>&- &
DTS_PID=$!
cleanup() {
    [ -z "$DOLBY_PID" ] || kill -TERM "$DOLBY_PID" 2>/dev/null
    [ -z "$DTS_PID" ] || kill -TERM "$DTS_PID" 2>/dev/null
    [ -z "$DOLBY_PID" ] || wait "$DOLBY_PID" 2>/dev/null
    [ -z "$DTS_PID" ] || wait "$DTS_PID" 2>/dev/null
}
trap cleanup EXIT
trap 'exit 0' HUP INT TERM
wait "$DOLBY_PID"
DOLBY_PID=
wait "$DTS_PID"
DTS_PID=
