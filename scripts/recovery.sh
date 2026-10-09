#!/system/bin/sh

# Only bounded checks after boot, resume or a service restart. Nothing queries the HAL
# while the event reader is idle, and neither the reader nor sleeps hold a wake lock.
wait_enabled() {
    remaining=$1
    while [ "$remaining" -gt 0 ]; do
        enabled || return 1
        if [ "$remaining" -gt 5 ]; then chunk=5; else chunk=$remaining; fi
        sleep "$chunk"
        remaining=$((remaining - chunk))
    done
    enabled
}

reconcile() {
    reason=$1
    case "$reason" in
        FrameworkRestart|AudioRestart)
            delays="0 5 15 40"
            log "$reason: checking BYPASS now, then at +5s, +20s and +60s."
            ;;
        *)
            delays="0 5 15"
            log "$reason: checking BYPASS now, then at +5s and +20s."
            ;;
    esac
    mode=
    # No infinite retries if the HAL is unavailable or rejects the setting.
    for delay in $delays; do
        wait_enabled "$delay" || return 1
        mode=$(read_mode)
        if [ -z "$mode" ]; then
            log "$reason: audio service unavailable."
            continue
        fi
        if [ "$mode" != 6 ]; then
            log "$reason: applying BYPASS; observed hdmi_format=$mode"
            timeout 5 /system/bin/aparam set 0 hdmi_format=6 >> "$MODDIR/dolby.log" 2>&1
            result=$?
            mode=$(read_mode)
            log "$reason: set exit=$result; readback hdmi_format='$mode'"
        fi
    done
    if [ "$mode" = 6 ]; then
        log "$reason checks complete: hdmi_format=6. Waiting for an event."
    else
        log "$reason checks failed: mode='$mode'. Will retry on the next event."
    fi
}

# logcat brief format retains tags, so unrelated numeric events cannot look
# like screen-on. Native audio service restarts are reported by AudioService;
# a framework restart is also covered if the old AudioService died with it.
event_reason() {
    case "$1" in
        I/power_screen_state\(*\):\ \[1,*) echo Resume ;;
        I/boot_progress_enable_screen\(*\):\ *) echo FrameworkRestart ;;
        E/AudioService\(*\):\ Audioserver\ started.) echo AudioRestart ;;
        *) return 1 ;;
    esac
}
