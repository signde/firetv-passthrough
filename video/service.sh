#!/system/bin/sh
OWNER=$1
M=$OWNER/video
umask 077
. "$OWNER/config.sh"
. "$M/verify-device.sh"
if ! verify_video_device; then
    echo UNSUPPORTED > "$M/fel-status.txt"
    echo UNSUPPORTED > "$M/mvc3d-status.txt"
    exit 0
fi
# MVC 3D frame packing: its own one-shot loader, whatever FEL does.
if [ "$ENABLE_MVC3D" = 1 ]; then
    /data/adb/magisk/busybox sh "$M/mvc3d-service.sh" "$M" > "$M/mvc3d.log" 2>&1 &
else
    echo DISABLED > "$M/mvc3d-status.txt"
fi
if [ "$ENABLE_FEL" != 1 ]; then
    echo DISABLED > "$M/fel-status.txt"
    exit 0
fi
exec /data/adb/magisk/busybox sh "$M/fel-service.sh" "$M" > "$M/fel.log" 2>&1
