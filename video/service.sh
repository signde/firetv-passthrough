#!/system/bin/sh
OWNER=$1
M=$OWNER/video
umask 077
. "$OWNER/config.sh"
. "$M/verify-device.sh"
if ! verify_video_device; then
    echo UNSUPPORTED > "$M/fel-status.txt"
    exit 0
fi
if [ "$ENABLE_FEL" != 1 ]; then
    echo DISABLED > "$M/fel-status.txt"
    exit 0
fi
exec /data/adb/magisk/busybox sh "$M/fel-service.sh" "$M" > "$M/fel.log" 2>&1
