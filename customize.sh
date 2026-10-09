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
# Do this last: a rejected package must leave existing modules enabled.
. "$MODPATH/scripts/legacy.sh"
disable_legacy_modules /data/adb || abort "Could not disable legacy modules. Installation cancelled."
ui_print "Verified $FIRMWARE_PROFILE. Installing Dolby and DTS passthrough together."
ui_print "Old Dolby/DTS modules, if present, are disabled. Reboot before playback."
ui_print "Experimental unified package: on-device testing is still pending."
ui_print "Allow about a minute after reboot. Disable/remove and reboot to undo."
