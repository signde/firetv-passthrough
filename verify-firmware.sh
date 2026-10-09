#!/system/bin/sh
# Select device/SDK first, then require one complete reviewed library set.
verify_firmware() {
    FIRMWARE_PROFILE=
    AUDIO_SERVICE=
    AUDIO_AGENT=
    case "$(getprop ro.product.device):$(getprop ro.build.version.sdk)" in
        gazelle:28)
            AUDIO_SERVICE=fireos.hardware.audio@2.0-service
            AUDIO_AGENT=runtime.js
            profiles="$1/firmware/ps*.sha256"
            ;;
        karat:30)
            AUDIO_SERVICE=fireos.hardware.audio.service
            AUDIO_AGENT=runtime-karat.js
            profiles="$1/firmware/karat-*.sha256"
            ;;
        *) return 1 ;;
    esac
    for profile in $profiles; do
        [ -f "$profile" ] || continue
        if sha256sum -c "$profile" >/dev/null 2>&1; then
            FIRMWARE_PROFILE=${profile##*/}
            FIRMWARE_PROFILE=${FIRMWARE_PROFILE%.sha256}
            return 0
        fi
    done
    return 1
}
