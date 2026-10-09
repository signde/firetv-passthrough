#!/system/bin/sh
# Magisk executes this with BusyBox ash standalone mode. No listening server.
: "${MODDIR:?Run through the module service.sh}"
. "$MODDIR/verify-firmware.sh"
umask 077
exec 9>/dev/firetv_dtshd.lock
flock -n 9 || exit 0
STATE=/dev/firetv_dtshd
mkdir -p "$STATE"
[ ! -f "$MODDIR/dts.log" ] || mv -f "$MODDIR/dts.log" "$MODDIR/previous-dts.log"
: > "$MODDIR/dts.log"
log() {
    size=$(wc -c < "$MODDIR/dts.log" 2>/dev/null)
    [ "${size:-0}" -lt 65536 ] || mv -f "$MODDIR/dts.log" "$MODDIR/dts.log.1"
    echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$MODDIR/dts.log"
}
status() { echo "$*" > "$MODDIR/status.txt"; }
enabled() { [ ! -e "$MODDIR/disable" ] && [ ! -e "$MODDIR/remove" ]; }
verify() {
    verify_firmware "$MODDIR" &&
    (cd "$MODDIR" && sha256sum -c payload.sha256 >/dev/null 2>&1)
}
pause_enabled() {
    left=$1
    while [ "$left" -gt 0 ]; do
        enabled || return 1
        sleep 1
        left=$((left - 1))
    done
}
APARAM=/system/bin/aparam
ulimit -c 0
mode() {
    timeout 5 "$APARAM" get 0 hdmi_format 2>/dev/null |
        tr -d '\r' | sed -n 's/^hdmi_format=\([0-9][0-9]*\)$/\1/p'
}
INJECTOR=
LOGGER=
FIFO=
cleanup() {
    [ -z "$INJECTOR" ] || kill -TERM "$INJECTOR" 2>/dev/null
    [ -z "$LOGGER" ] || kill "$LOGGER" 2>/dev/null
    [ -z "$FIFO" ] || rm -f "$FIFO"
}
trap cleanup EXIT
trap 'exit 0' HUP INT TERM
MODULE_VERSION=$(sed -n 's/^version=//p' "$MODDIR/module.prop")
status "WAITING_FOR_BOOT"
verify || { log "Unsupported firmware or corrupt payload; refusing attachment."; status "UNSUPPORTED"; exit 1; }
[ ! -e "$MODDIR/blocked" ] || { log "Restart guard is latched; inspect logs and remove blocked only after diagnosing."; status "BLOCKED"; exit 1; }
count=0
while [ "$(getprop sys.boot_completed)" != 1 ]; do
    pause_enabled 2 || exit 0
    count=$((count + 1))
    [ "$count" -lt 150 ] || { status "BOOT_TIMEOUT"; exit 1; }
done
# Let Fire OS and the Dolby worker reconcile the saved output mode.
log "Boot complete; settling for 65 seconds."
pause_enabled 65 || exit 0
rapid=0
while enabled; do
    verify || { status "UNSUPPORTED"; exit 1; }
    pid=$(pidof "$AUDIO_SERVICE")
    case "$pid" in ''|*' '*) pause_enabled 5 || exit 0; continue ;; esac
    # Refuse duplicate attachment, even if a supervisor was killed/restarted.
    start=$(awk '{print $22}' "/proc/$pid/stat" 2>/dev/null)
    signature="$pid:$start"
    [ "$(cat "$STATE/attached" 2>/dev/null)" != "$signature" ] || {
        log "This HAL instance was already attached; refusing duplicate hooks."
        status "ALREADY_ATTACHED_OR_RESTART_REQUIRED pid=$pid"
        exit 1
    }
    # Only bounded startup/service-restart checks, never periodic audio queries.
    current=$(mode)
    if [ "$current" != 6 ]; then
        log "HAL startup: applying required HDMI BYPASS (observed '$current')."
        timeout 5 "$APARAM" set 0 hdmi_format=6 >> "$MODDIR/dts.log" 2>&1
        current=$(mode)
    fi
    [ "$current" = 6 ] || { log "BYPASS readback failed."; status "BYPASS_FAILED"; exit 1; }
    [ "$(pidof "$AUDIO_SERVICE")" = "$pid" ] || continue
    echo "$signature" > "$STATE/attached"
    rm -f "$STATE/ready" "$STATE/fatal"
    log "Verified $FIRMWARE_PROFILE; using $AUDIO_AGENT for $AUDIO_SERVICE."
    status "ATTACHING pid=$pid profile=$FIRMWARE_PROFILE"
    FIFO="$STATE/events.fifo"
    rm -f "$FIFO"
    mkfifo "$FIFO" || exit 1
    (
        while IFS= read -r line; do
            log "$line"
            case "$line" in
                *'"kind":"ready"'*) touch "$STATE/ready"; status "ACTIVE pid=$pid version=$MODULE_VERSION" ;;
                *'DTS_FATAL'*) touch "$STATE/fatal"; status "ATTACH_FAILED pid=$pid" ;;
                *'"kind":"trial-error"'*) status "STREAM_ERROR pid=$pid (stop playback; see dts.log)" ;;
            esac
        done < "$FIFO"
    ) 9>&- &
    LOGGER=$!
    began=$(date +%s)
    "$MODDIR/bin/frida-inject" -p "$pid" -s "$MODDIR/$AUDIO_AGENT" -R qjs </dev/null > "$FIFO" 2>&1 9>&- &
    INJECTOR=$!
    attempt=0
    while [ ! -f "$STATE/ready" ] && kill -0 "$INJECTOR" 2>/dev/null; do
        [ ! -e "$STATE/fatal" ] || break
        attempt=$((attempt + 1))
        [ "$attempt" -le 30 ] || break
        pause_enabled 1 || exit 0
    done
    if [ ! -f "$STATE/ready" ]; then
        log "Attachment did not become ready; stopping this attempt."
        status "ATTACH_FAILED pid=$pid"
        exit 1
    fi
    log "Attached to HAL pid=$pid. Waiting for process exit; no idle polling."
    # The runtime blocks on process lifetime. No Mac, ADB or open listener needed.
    wait "$INJECTOR"
    result=$?
    INJECTOR=
    wait "$LOGGER"
    LOGGER=
    rm -f "$FIFO"
    FIFO=
    elapsed=$(( $(date +%s) - began ))
    log "Runtime ended exit=$result after ${elapsed}s."
    enabled || exit 0
    if [ "$(pidof "$AUDIO_SERVICE")" = "$pid" ]; then
        status "RUNTIME_LOST_RESTART_REQUIRED pid=$pid"
        exit 1
    fi
    if [ "$elapsed" -lt 120 ]; then rapid=$((rapid + 1)); else rapid=0; fi
    if [ "$rapid" -ge 3 ]; then
        touch "$MODDIR/blocked"
        log "Three rapid HAL exits; latched attachment guard."
        status "BLOCKED"
        exit 1
    fi
    status "WAITING_FOR_AUDIO_RESTART"
    pause_enabled 5 || exit 0
done
