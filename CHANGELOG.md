# Changelog

## 0.2.5 (experimental)

- Adds hardware-tested Dolby Vision Profile 7 FEL support on six Cube 3 builds: PS7713.5443N, PS7714.5503N, PS7714.5506N, PS7714.5507N, PS7715.5585N and PS7716.5665N. Builds 5585 and 5665 share one verified FEL helper.
- Enables the existing RAW TrueHD timing, packing, HDMI capability and microphone/DSP fixes for the audio libraries in build 5443 and all three 7.7.1.4 builds. Builds 5585 and 5665 already matched the audio checks in v0.2.4.

## 0.2.5-test.6 (unreleased)

- Adds Cube 3 FEL support on Fire OS 7.7.1.6 build PS7716.5665N. This build shares the 7.7.1.5 FEL helper and audio corrections.

## 0.2.5-test.5 (unreleased)

- Adds Cube 3 FEL support on Fire OS 7.7.1.5 build PS7715.5585N. This build uses the existing TrueHD and microphone/DSP corrections.

## 0.2.5-test.4 (unreleased)

- Adds Cube 3 FEL support on Fire OS 7.7.1.4 build PS7714.5507N. Its audio libraries match the tested PS7714.5506N build.

## 0.2.5-test.3 (unreleased)

- Adds Cube 3 FEL support on Fire OS 7.7.1.4 build PS7714.5506N. Its audio libraries match the tested PS7714.5503N build.

## 0.2.5-test.2 (unreleased)

- Adds Cube 3 FEL support on Fire OS 7.7.1.4 (PS7714.5503N). This build uses the same audio libraries as PS7713.5443N for passthrough and the TrueHD corrections.

## 0.2.5-test.1 (unreleased)

- Adds Cube 3 FEL support on Fire OS 7.7.1.3 (PS7713.5443N). Enables the Cube’s TrueHD timing, packing, HDMI capability and microphone/DSP corrections for its reviewed audio libraries.

## 0.2.4 (experimental)

- Cube 3 gains RAW TrueHD timing, packing and HDMI capability fixes, plus FEL support on PS7711.5272N, PS7712.5371N, PS7717.5746N and PS7718.5826N.
- Stick 4K Max 2 retains the fixes from 0.2.3-rc.1. Mode 6 survives frame-rate changes, and apps can recover DD/DD+ passthrough when Fire OS omits formats advertised by the HDMI sink.

## 0.2.4-truehd-test.7 (unreleased experimental test build)

- Adds Cube 3 FEL support for Fire OS 7.7.1.2 (PS7712.5371N, kernel 5.4.125-ab26). The existing audio profile covers passthrough, the microphone DSP fix and the RAW TrueHD corrections on this build.

## 0.2.4-truehd-test.6 (unreleased experimental test build)

- Adds Cube 3 FEL support for Fire OS 7.7.1.1 (PS7711.5272N, kernel 5.4.125-ab20). The existing audio profile covers passthrough, the microphone DSP fix and the RAW TrueHD corrections on this build.

## 0.2.4-truehd-test.5 (unreleased experimental test build)

- Adds a Cube 3 FEL profile for PS7718.5826N and kernel 5.4.125-ab35. Its audio libraries match the existing passthrough and voice DSP checks.

## 0.2.4-truehd-test.4 (unreleased experimental test build)

- Adds a Cube 3 FEL profile for PS7717.5746N and kernel 5.4.125-ab30. This build matches the existing passthrough and voice DSP library checks.

## 0.2.4-truehd-test.3 (unreleased experimental test build)

- Combines sample rates across matching HDMI descriptors for RAW TrueHD.
- Limits Gazelle TrueHD encoder input batches to avoid the packing retry loop seen with sparse test signals.
- Scales the 130 ms correction by sample rate, including 96 kHz. The 48 kHz behavior is unchanged.
- Corrects the missing playback-position adjustment for RAW TrueHD at all valid sample rates on reviewed PS7702.4965N and PS7717.5741N libraries.
- Keeps existing timing for IEC and other codecs, and rejects invalid sample rates. Karat is unchanged.
- Based on 0.2.2; includes no HDR output changes or pending Karat fixes.

## 0.2.3-rc.1 (experimental prerelease)

- Refresh stale Android HDMI capabilities after audio-hook attachment when needed, without changing saved surround preferences.
- Retains Karat HDMI bypass when Fire OS restores mode 5 during frame-rate changes.
- Restores missing DD/DD+ HDMI capability replies in bypass mode, limited to formats supported by the connected display.
- Uses the existing on-device audio hook, with no periodic HDMI polling.
- Gazelle audio/FEL and the supported firmware profiles are unchanged. HDR output enhancements are not included.
- Tested on Karat RS8182.3811N with Prime frame-rate switching, DD/DD+ passthrough, reboot and audio-service recovery. The intermittent Peacock live-stream startup stall remains unresolved.

## 0.2.2 (experimental)

- Fixes Gazelle FEL flicker while preserving protection against green flashes during seeking.
- Tracks composer-mode writes and applies necessary corrections before the frame configuration is applied.
- Tested in Kodi FEL and VLC FEL on 7.7.0.2 (PS7702.4965N) and 7.7.1.7 (PS7717.5741N), including pause/resume and seeking.
- Audio handling, Karat and firmware compatibility are unchanged. HDR output enhancements are not included.

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
