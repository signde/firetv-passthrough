#!/system/bin/sh
M=$1
OWNER=${M%/*}
umask 077
exec 9>/dev/firetv_fel_start.lock
/data/adb/magisk/busybox flock -n 9 || exit 0
status() { echo "$*" > "$M/fel-status.txt"; echo "$*"; }
fail() { [ ! -e /dev/dvfel ] || chmod 000 /dev/dvfel /dev/dvfel.* 2>/dev/null; status "$*"; exit 1; }
stop_requested() { [ -e "$OWNER/disable" ] || [ -e "$OWNER/remove" ]; }
status WAITING
# Do not block Android boot or initialize Dolby by launching a hidden player.
i=0
while [ "$i" -lt 120 ]; do
 stop_requested && fail DISABLED
 if [ "$(getprop sys.boot_completed)" = 1 ] && grep -q '^dovi ' /proc/modules; then break; fi
 sleep 2; i=$((i+1))
done
[ "$i" -lt 120 ] || fail 'WAITING_FAILED: stock Dolby driver unavailable'
. "$M/verify-device.sh"
verify_video_device || fail UNSUPPORTED
if [ -d /sys/module/dvfel ]; then
 fail 'ALREADY_LOADED: reboot to replace the temporary FEL helper'
fi
# Kernel code/address/layout guards and stock Dolby instruction guards are
# validated by the helper before any patch is activated.
insmod "$M/fel/$FEL_PROFILE/dvfel.ko" open_driver_fixed=1 debug=0 residual_bypass=1 fel_composer_mode=1 || fail 'LOAD_FAILED: inspect kernel log; no forced load attempted'
# Android creates the device node asynchronously after misc_register returns.
i=0
while [ ! -c /dev/dvfel ] && [ "$i" -lt 50 ]; do
 sleep 0.1; i=$((i+1))
done
[ -c /dev/dvfel ] || fail DEVICE_NODE_TIMEOUT
# Match the existing validated, opt-in player settings.
for setting in wait_el:1 residual_unpaired:1 coherent_bl_fallback:1 seek_wait_ms:200 blank_seek_gaps:0; do
 name=${setting%:*};value=${setting#*:}
 echo "$value" > "/sys/module/dvfel/parameters/$name" || fail CONFIGURATION_FAILED
done
# All application access is denied until the complete configuration is ready.
chcon u:object_r:sysfs_devices_system_cpu:s0 /sys/module/dvfel/version || fail VERSION_CONTEXT_FAILED
for setting in seek_drain_ms:180 sc2_buffers:7 sc2_images:2 seek_preroll:1 hold_images:1 hold_max:3 trace_pacing:0; do
 name=${setting%:*}; value=${setting#*:}
 setprop "debug.dvfp.$name" "$value" || fail PLAYER_CONFIGURATION_FAILED
done
. "$OWNER/scripts/fel-access.sh"
provision_fel_access || fail PLAYER_ACCESS_FAILED
status "READY: $(cat /sys/module/dvfel/version), shared FEL interface"
# Session ownership and buffer pins are managed by the helper. Never force
# rmmod on shutdown, disable or app exit; reboot removes all in-memory hooks.
