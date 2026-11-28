#!/system/bin/sh
# Karat requires the generated /system/bin/aparam Magisk overlay.
[ "$BOOTMODE" = true ] || abort "Install from Magisk in Fire OS."
. "$MODPATH/verify-firmware.sh"
verify_firmware "$MODPATH" || abort "Unsupported audio libraries."
(cd "$MODPATH" && sha256sum -c payload.sha256 >/dev/null 2>&1) || abort "Module payload checksum mismatch."
[ -x /system/bin/aparam ] || abort "Required audio parameter utility is missing."
case "$(getprop ro.product.device)" in
    gazelle) ;;
    karat)
        mkdir -p "$MODPATH/system/bin" || abort "Cannot create utility overlay."
        sh "$MODPATH/scripts/patch_karat_aparam.sh" /system/bin/aparam "$MODPATH/system/bin/aparam" ||
            abort "Unsupported Karat utility or overlay patch failed."
        ;;
    *) abort "Supported devices: gazelle and karat only." ;;
esac
set_perm_recursive "$MODPATH" 0 0 0755 0644 || abort "Cannot set module permissions."
set_perm "$MODPATH/service.sh" 0 0 0755 || abort "Cannot set service permissions."
set_perm "$MODPATH/bin/frida-inject" 0 0 0700 || abort "Cannot set runtime permissions."
if [ -f "$MODPATH/system/bin/aparam" ]; then
    set_perm "$MODPATH/system/bin/aparam" 0 2000 0755 || abort "Cannot set overlay permissions."
fi
# MVC 3D: the SELinux types and rules for its device nodes, applied by Magisk early in boot; a live policy
# change right after boot can crash system_server (scripts/mvc3d-access.sh). The decoder input's rules copy
# its stock type's character device rules from the running policy; with none found they are left out and the
# loader applies them live once the system has settled, as it does for a Magisk that ignores sepolicy.rule.
if [ "$(getprop ro.product.device)" = gazelle ] && [ -f "$MODPATH/config.sh" ] &&
    [ -f "$MODPATH/video/verify-device.sh" ] && [ -f "$MODPATH/scripts/mvc3d-access.sh" ]; then
    . "$MODPATH/config.sh"
    . "$MODPATH/video/verify-device.sh"
    if [ "$ENABLE_MVC3D" = 1 ] && verify_video_device && [ -f "$MODPATH/video/mvc3d/$FEL_PROFILE/mvc3d.ko" ]; then
        OWNER=$MODPATH
        . "$MODPATH/scripts/mvc3d-access.sh"
        t=
        [ "$ENABLE_MVC3D_APP_DECODER" = 0 ] ||
            t=$(ls -Z /dev/amstream_vbuf 2>/dev/null | sed -n 's/^[^:]*:[^:]*:\([^:]*\):.*/\1/p')
        mvc3d_policy_rules "$t" > "$MODPATH/sepolicy.rule" || abort "Cannot write the MVC 3D SELinux rules."
        stock=$(grep "^allow " "$MODPATH/sepolicy.rule" | grep -v "^allow appdomain " | grep -c .)
        if [ -n "$t" ] && [ "$stock" -lt 1 ]; then
            mvc3d_policy_rules > "$MODPATH/sepolicy.rule" || abort "Cannot write the MVC 3D SELinux rules."
            ui_print "MVC 3D: no stock rules found for the decoder input; the loader applies them after boot."
        fi
    fi
fi
# Do this last: a rejected package must leave existing modules enabled.
. "$MODPATH/scripts/legacy.sh"
disable_legacy_modules /data/adb || abort "Could not disable legacy modules. Installation cancelled."
ui_print "Verified $FIRMWARE_PROFILE. Installing Dolby and DTS passthrough together."
ui_print "Old Dolby/DTS/video modules, if present, are disabled. Reboot before playback."
ui_print "Gazelle FEL requires a separately verified kernel; Karat is audio-only."
ui_print "Allow about a minute after reboot. Disable/remove and reboot to undo."
