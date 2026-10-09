#!/system/bin/sh
# Magisk may also have an older module staged for the next reboot.
legacy_enabled() {
    for parent in modules modules_update; do
        for id in gazelle_ddplus_bypass firetv_dtshd_passthrough; do
            legacy="$1/$parent/$id"
            if [ -f "$legacy/module.prop" ] && [ ! -e "$legacy/disable" ] && [ ! -e "$legacy/remove" ]; then
                return 0
            fi
        done
    done
    return 1
}

disable_legacy_modules() {
    # Production paths under /data/adb have no whitespace. Record only markers
    # created here, so rollback never re-enables an already disabled module.
    changed=
    for parent in modules modules_update; do
        for id in gazelle_ddplus_bypass firetv_dtshd_passthrough; do
            legacy="$1/$parent/$id"
            [ -f "$legacy/module.prop" ] || continue
            [ ! -e "$legacy/disable" ] && [ ! -e "$legacy/remove" ] || continue
            if ! touch "$legacy/disable"; then
                for marker in $changed; do rm -f "$marker"; done
                return 1
            fi
            changed="$legacy/disable $changed"
        done
    done
    return 0
}
