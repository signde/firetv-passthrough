# Fire TV Audio Passthrough

One Magisk module for **DD, DD+, DD+ Atmos, DTS-HD MA and DTS:X passthrough** on
**Fire TV Cube 3 (Gazelle, Fire OS 7)** and **Fire TV Stick 4K Max 2 (Karat, Fire OS 8)**.

**v0.1.0 experimental release.**
This combines Dolby v0.3.2 and DTS v0.2.0, replacing both separate modules.

Combined-module smoke tests passed on Gazelle PS7717.5741 and Karat RS8182.3811N:
DTS-HD MA/DTS:X playback, reboot, sleep/wake recovery and audio-service restart.
DD+ Atmos was also verified on Gazelle; Karat Dolby playback remains unverified
with the combined package.

## Supported firmware

The tables below carry forward results from the separate modules. **User-tested**
means playback-tested with those modules; **static review** means library
compatibility was checked without playback testing that build. Installation
requires a matching reviewed audio-library set; Karat also checks its audio utility.

### Cube 3 / Gazelle

| Fire OS | Build(s) | Status |
| --- | --- | --- |
| 7.6.8.8 | PS7688.4591 | Static review |
| 7.6.9.0 | PS7690.4714, PS7690.4716 | Static review |
| 7.6.9.6 | PS7696.5226, PS7696.5229 | Static review |
| 7.6.9.9 | PS7699.4894, PS7699.4896 | Static review |
| 7.7.0.2 | PS7702.4965 | User-tested |
| 7.7.0.4 | PS7704.5024 | Static review |
| 7.7.0.6 | PS7706.5106 | Static review |
| 7.7.0.7 | PS7707.5376 | Static review |
| 7.7.1.0 | PS7710.6003 | Static review |
| 7.7.1.1 | PS7711.5272 | Static review |
| 7.7.1.2 | PS7712.5371 | Static review |
| 7.7.1.3 | PS7713.5443 | Static review |
| 7.7.1.4 | PS7714.5503, PS7714.5507 | Static review |
| 7.7.1.4 | PS7714.5506 | User-tested |
| 7.7.1.5 | PS7715.5585 | Static review |
| 7.7.1.6 | PS7716.5665 | Static review |
| 7.7.1.7 | PS7717.5741 | User-tested |

### Stick 4K Max 2 / Karat

| Fire OS | Build(s) | Status |
| --- | --- | --- |
| 8.1.4.5 | RS8145.3070N | Static review |
| 8.1.4.9 | RS8149.3133N | Static review |
| 8.1.5.3 | RS8153.3202N | Static review |
| 8.1.5.5 | RS8155.3474N | Static review |
| 8.1.5.8 | RS8158.4105N | Static review |
| 8.1.6.0 | RS8160.3372N, RS8160.3380N | Static review |
| 8.1.6.6 | RS8166.3482N | Static review |
| 8.1.6.9 | RS8169.3556N | Static review |
| 8.1.7.4 | RS8174.3641N, RS8174.3648N | Static review |
| 8.1.8.0 | RS8180.3729N, RS8180.3739N | Static review |
| 8.1.8.2 | RS8182.3811N | User-tested |

RS8185.3879N has not been reviewed.

## App results

Observed output with the tested samples:

| App | Cube 3 / Gazelle | Stick 4K Max 2 / Karat |
| --- | --- | --- |
| Nova, system passthrough | ✅ DTS-HD MA, DTS:X | ✅ DTS-HD MA, DTS:X |
| Emby 3.5.63 | ✅ DTS-HD MA, DTS:X | ✅ DTS-HD MA, DTS:X |
| Kodi 21 | ✅ DTS-HD MA, DTS:X | ✅ DTS-HD MA, DTS:X |
| Plezy 2.22.0 | ✅ DTS-HD MA, DTS:X | Untested |
| Wholphin 1.0.8 | ✅ DTS-HD MA, DTS:X | Untested |
| Plex 2026.19.1 | ❌ AAC transcode | ✅ DTS-HD MA, DTS:X |
| Jellyfin Android TV 0.19.10 | ⚠️ DTS core or ❌ AAC transcode | ⚠️ DTS core |

Karat results use RS8182.3811N, Nova 6.4.3, Emby 3.5.63 (`com.mb.android`)
and Kodi 21.3. Gazelle's Plex/Jellyfin results are from PS7702. Kodi uses its own
DTS packer and does not need the DTS fix. Plezy and Wholphin were tested on
PS7717 with the combined module; Plezy supplied already-packed audio, while
Wholphin used the patched system packer. App/server transcoding cannot be
reversed by the module.

## Installation

You need root, Magisk and supported firmware. DTS-HD requires a receiver or
TV/eARC chain advertising eight-channel DTS-HD at 192 kHz.

1. Set Fire OS audio to **Best Available** and enable passthrough in your player.
   In Nova, choose **system passthrough**.
2. Install `firetv-passthrough-v0.1.0.zip` in Magisk, then reboot.
3. Allow about one minute after boot before starting playback.

Use the module ZIP, not GitHub's automatic source ZIP. The installer disables the
old Dolby and DTS modules if present. Reboot to switch over, then remove those
disabled modules after confirming playback works.

To uninstall, disable/remove this module and reboot. To roll back to the separate
modules, disable this one, re-enable the old modules and reboot. Disable this
module before a firmware update and check compatibility before enabling it again.

## Troubleshooting

From a root shell:

```sh
aparam get 0 hdmi_format
cat /data/adb/modules/firetv_passthrough/status.txt
tail -40 /data/adb/modules/firetv_passthrough/dolby.log
tail -40 /data/adb/modules/firetv_passthrough/dts.log
```

Expect `hdmi_format=6` and DTS status `ACTIVE`. If a stream fails, stop and reopen
it. Repeated audio-service failures stop further DTS attachment; inspect the logs
before retrying. Do not run older trial hooks alongside this module.

The tested DTS profile is 48 kHz with 512-sample core frames. Other source profiles
and receiver hotplug during playback remain untested.

## How it works

The Dolby component maintains HDMI bypass at boot and after wake/restart events,
with brief follow-up checks. The DTS component replaces the system packing path
in memory, preserving HD audio in IEC61937 bursts. Neither component polls HDMI
while idle. Vendor audio libraries and app APKs are not replaced.

Karat also receives a Magisk overlay correcting the audio utility's exit crash,
generated from the device's own file. No firmware binaries are bundled. Frida
runs on-device without ADB or a computer and captures no audio.

## License

Project code is MIT licensed. Frida retains its own licenses; see
[NOTICE.md](NOTICE.md). Published module packages must be accompanied by the
Frida source companion archive, which is not a Magisk module.
See [CHANGELOG.md](CHANGELOG.md) for version history.
