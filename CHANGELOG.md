# Changelog

## 0.2.1 (experimental)

- Fixes a Gazelle regression in the TrueHD microphone correction: temporary audio outputs could trigger DSP errors and sustained high CPU usage during streaming playback.
- Coordinates DSP reference changes with actual playback and teardown; retains mode 6 and TrueHD passthrough.
- Tested on PS7717 with ESPN audio transitions and TrueHD pause/resume, seeking and stopping. Karat and FEL are unchanged. HDR output enhancements are not included.

## 0.2.0 (experimental candidate)

- Renames the module to Fire TV AV; retains the existing Magisk ID for upgrades.

- Combines audio passthrough and Gazelle hardware FEL.
- Retains Gazelle audio bypass through refresh-rate changes.
- Recovers when Fire OS replaces the audio service during startup attachment, with bounded retries.
- Corrects the Gazelle voice DSP reference format on audio-output startup to prevent the TrueHD microphone error storm.
- Exposes a shared, opt-in FEL interface for compatible players without an app whitelist or installation-order requirement.
- Enforces ioctl session ownership, serializes queue/stop operations and rejects invalid inputs.
- Validated Kodi FEL network playback, MEL/P5/P8 fallback and TrueHD on PS7717.
  4K60 frame pacing remains deferred.
- Documents Kodi audio delay supervision and multichannel PCM settings.
- Migrates the standalone video module. Karat remains audio-only.

## 0.1.0 (experimental, 2026-10-09)

- Combines Dolby v0.3.2 and DTS v0.2.0 into one Magisk module for Gazelle and Karat.
- Keeps HDMI bypass recovery, the Karat audio utility fix, and both DTS packers.
- Retains the reviewed firmware profiles and unknown-library rejection.
- Disables the separate modules during installation; reboot completes migration.

Smoke-tested on Gazelle PS7717.5741 and Karat RS8182.3811N, including DTS-HD MA,
DTS:X and boot/wake/audio-service recovery. Gazelle DD+ Atmos also passed; Karat
Dolby playback remains unverified with the combined package. The broader firmware
and app tables retain results from the separate modules.
