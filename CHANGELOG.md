# Changelog

## 0.1.0 (experimental, 2026-10-09)

- Combines Dolby v0.3.2 and DTS v0.2.0 into one Magisk module for Gazelle and Karat.
- Keeps HDMI bypass recovery, the Karat audio utility fix, and both DTS packers.
- Retains the reviewed firmware profiles and unknown-library rejection.
- Disables the separate modules during installation; reboot completes migration.

Smoke-tested on Gazelle PS7717.5741 and Karat RS8182.3811N, including DTS-HD MA,
DTS:X and boot/wake/audio-service recovery. Gazelle DD+ Atmos also passed; Karat
Dolby playback remains unverified with the combined package. The broader firmware
and app tables retain results from the separate modules.
