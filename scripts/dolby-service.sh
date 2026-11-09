#!/system/bin/sh

# Magisk runs this in its BusyBox ash standalone environment.
: "${MODDIR:?Run through the module service.sh}"
umask 077
# Kernel-backed lock is released even if the process is killed.
exec 9> /dev/gazelle_ddplus_bypass.lock
flock -n 9 || exit 0
[ ! -f "$MODDIR/dolby.log" ] || mv -f "$MODDIR/dolby.log" "$MODDIR/previous-dolby.log"
# Open the log per message so rotation also bounds a long-running session.
: > "$MODDIR/dolby.log"
log() {
    size=$(wc -c < "$MODDIR/dolby.log" 2>/dev/null)
    if [ "${size:-0}" -ge 65536 ]; then
        mv -f "$MODDIR/dolby.log" "$MODDIR/dolby.log.1"
    fi
    echo "$(date '+%Y-%m-%d %H:%M:%S') [$$] $*" >> "$MODDIR/dolby.log"
}
enabled() { [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ]; }
read_mode() {
    timeout 5 /system/bin/aparam get 0 hdmi_format 2>/dev/null |
        tr -d '\r' | sed -n 's/^hdmi_format=\([0-9][0-9]*\)$/\1/p'
}

DEVICE=$(/system/bin/getprop ro.product.device)
log "Startup (Dolby component): $DEVICE $(/system/bin/getprop ro.build.display.id)"
case "$DEVICE" in
    gazelle) ;;
    karat)
        expected=690cfd8c33e3c02d68c7e0d1c51415530907bcf41f1cf8784186ba17c897e4f2
        actual=$(sha256sum /system/bin/aparam)
        actual=${actual%% *}
        [ "$actual" = "$expected" ] || {
            log "Corrected Karat aparam overlay is unavailable; exiting."
            exit 1
        }
        ;;
    *) log "Unsupported device; exiting."; exit 1 ;;
esac
[ -x /system/bin/aparam ] || { log "aparam is unavailable."; exit 1; }

# This late-start service does not block Android boot. Give boot five minutes.
attempt=0
while [ "$(/system/bin/getprop sys.boot_completed)" != 1 ]; do
    enabled || exit 0
    attempt=$((attempt + 1))
    [ "$attempt" -le 150 ] || { log "Boot completion timed out."; exit 1; }
    sleep 2
done

# Fire OS applies saved audio preferences after sys.boot_completed becomes 1.
# On PS7702 this reset occurred after v0.1.0 had already exited successfully.
log "Boot complete; allowing 60 seconds for saved audio preferences."
settle=0
while [ "$settle" -lt 12 ]; do
    enabled || exit 0
    sleep 5
    settle=$((settle + 1))
done
. "$MODDIR/scripts/recovery.sh"

# One restart per few minutes. A display reset that merely probes an unsupported
# tunnel must not kill a stream that already recovered, and a restart must not
# retrigger itself when Fire OS rewrites mode 5 as the HAL comes back.
playback_active() {
    /system/bin/dumpsys media.audio_flinger 2>/dev/null |
        /system/bin/grep -q "Standby: no"
}
restart_blocked() {
    stamp=$(cat "$MODDIR/hal-restart.stamp" 2>/dev/null) || return 1
    case "$stamp" in ''|*[!0-9]*) return 1 ;; esac
    now=$(date +%s)
    [ $((now - stamp)) -lt 180 ]
}
recover_failed_open() {
    if restart_blocked; then
        log "HDMI open failed after a display reset. HAL restart suppressed; latch held."
        return 0
    fi
    if playback_active; then
        log "HDMI open failed after a display reset, but an output is active. Not restarting the audio HAL."
        return 0
    fi
    log "HDMI open failed after a display reset and no output is active. Restarting the audio HAL once."
    date +%s > "$MODDIR/hal-restart.stamp"
    /system/bin/setprop ctl.restart vendor.audio-hal
    attempt=0
    while [ "$attempt" -lt 20 ]; do
        enabled || return 1
        mode=$(read_mode)
        [ -n "$mode" ] && break
        sleep 1
        attempt=$((attempt + 1))
    done
    reconcile HalRestart
}

reconcile Boot || exit 0

# Watch screen-on, framework startup, audio-server recovery, and Fire OS
# writing hdmi_format=5. Gazelle rewrites that request in the HAL. Karat
# keeps Fire OS mode: bypass drops Kodi PCM and refuses Prime Atmos.
# Filtering occurs in logcat; no log clearing or periodic HDMI queries.
FIFO=/dev/firetv_audio_resume.$$.fifo
EVENT_PID=
cleanup() {
    if [ -n "$EVENT_PID" ]; then
        kill "$EVENT_PID" 2>/dev/null
        wait "$EVENT_PID" 2>/dev/null
    fi
    rm -f "$FIFO"
}
trap cleanup EXIT
trap 'exit 0' HUP INT TERM
mkfifo "$FIFO" || { log "Cannot create resume event pipe."; exit 1; }

while enabled; do
    # -T 1 also recovers the most recent event if the reader reconnects.
    # A replay may produce one harmless bounded reconciliation, not idle polling.
    # An open failure restarts the HAL only when it arrives in the two-second
    # burst after an hdmi_format=5 line, so a later tunnel probe cannot.
    # AudioOutputFormatManager is system_server, so its lines are in the system
    # buffer. main and events do not carry the hdmi_format=5 write.
    /system/bin/logcat -b events -b main -b system -v brief -T 1 \
        power_screen_state:I boot_progress_enable_screen:I AudioService:E \
        AudioOutputFormatManager:I APM_AudioPolicyManager:W AudioALSAStreamManager:E \
        AudioTunnelStreamOut:E AudioHwDevice:E MS12Server:W \
        '*:S' > "$FIFO" 2>> "$MODDIR/dolby.log" 9>&- &
    EVENT_PID=$!
    log "Recovery listener started; no periodic HDMI checks."
    while IFS= read -r event; do
        enabled || break
        case "$event" in
            *'ByPass don'\''t use Dolby StreamOut'*|*'Invalid format 0x24000000'*|*'Invalid format 0x24000001'*|*'SPDIFEncoder does not support format 0x2400000'*)
                # Prime asked for Atmos while bypass had disabled MS12, or the
                # policy probe rejected MAT. Request Fire OS mode 5. On Gazelle
                # the HAL rewrites that request to mode 6.
                log "Atmos open needs the Dolby decoder. Setting hdmi_format=5."
                timeout 5 /system/bin/aparam set 0 hdmi_format=5 >> "$MODDIR/dolby.log" 2>&1
                mode=$(read_mode)
                log "Dolby hold readback hdmi_format='${mode:-unset}'."
                ;;
            *'hdmi_format=5;'*)
                failed_open=0
                mat_rejected=0
                extra_reason=
                while IFS= read -r -t 2 extra; do
                    case "$extra" in
                        *'ByPass don'\''t use Dolby StreamOut'*|*'Invalid format 0x24000000'*|*'Invalid format 0x24000001'*|*'SPDIFEncoder does not support format 0x2400000'*)
                            mat_rejected=1
                            ;;
                        *'could not open output for device 400'*|*'TunnelStreamOut->set() fail'*)
                            failed_open=1
                            ;;
                        *'hdmi_format=5;'*) ;;
                        *)
                            matched=$(event_reason "$extra") || true
                            [ -n "$matched" ] && extra_reason=$matched
                            ;;
                    esac
                done
                if [ "$mat_rejected" = 1 ]; then
                    log "Atmos open needs the Dolby decoder. Setting hdmi_format=5."
                    timeout 5 /system/bin/aparam set 0 hdmi_format=5 >> "$MODDIR/dolby.log" 2>&1
                else
                    reconcile DisplayReset || break
                fi
                # Karat never writes bypass. Gazelle may restart the audio HAL
                # once when this burst fails to open output 400 and playback is idle.
                if [ "$failed_open" = 1 ] && [ "$DEVICE" != karat ]; then
                    recover_failed_open || break
                fi
                if [ -n "$extra_reason" ]; then
                    reconcile "$extra_reason" || break
                fi
                ;;
            *'could not open output for device 400'*|*'TunnelStreamOut->set() fail'*)
                ;;
            *)
                reason=$(event_reason "$event") || continue
                reconcile "$reason" || break
                ;;
        esac
    done < "$FIFO"
    kill "$EVENT_PID" 2>/dev/null
    wait "$EVENT_PID" 2>/dev/null
    EVENT_PID=
    enabled || break
    log "Event stream ended; reconnecting in 30 seconds."
    wait_enabled 30 || break
done
log "Module disabled or removed; monitoring stopped."
