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

reconcile Boot || exit 0

# Watch screen-on, framework startup, and audio-server recovery.
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
    /system/bin/logcat -b events -b main -v brief -T 1 \
        power_screen_state:I boot_progress_enable_screen:I AudioService:E '*:S' > "$FIFO" 2>> "$MODDIR/dolby.log" 9>&- &
    EVENT_PID=$!
    log "Recovery listener started; no periodic HDMI checks."
    while IFS= read -r event; do
        enabled || break
        reason=$(event_reason "$event") || continue
        reconcile "$reason" || break
    done < "$FIFO"
    kill "$EVENT_PID" 2>/dev/null
    wait "$EVENT_PID" 2>/dev/null
    EVENT_PID=
    enabled || break
    log "Event stream ended; reconnecting in 30 seconds."
    wait_enabled 30 || break
done
log "Module disabled or removed; monitoring stopped."
