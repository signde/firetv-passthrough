# Fire TV AV

One experimental Magisk module for audio passthrough on **Cube 3 / Gazelle** and
**Stick 4K Max 2 / Karat**, with hardware Dolby Vision FEL on supported Cube 3 builds.

**v0.2.2 experimental.** Replaces the audio module and the private
standalone video module. Karat remains audio-only.

## Gazelle TrueHD correction

Coordinates the voice DSP reference with audio-output opening, playback and
teardown, keeping mode 6 and TrueHD passthrough active. This fixes a regression
in the previous correction: brief audio-output changes could trigger microphone
errors and sustained high CPU usage, observed during ESPN playback.

The revised handling was tested on 7.7.1.7 with ESPN transitions and TrueHD
playback. It retains the reviewed library checks; the previous correction was
also tested on 7.7.0.2. Karat is unchanged.

Invoking Alexa during TrueHD can still trigger the microphone error storm.
That interaction and the unrelated Kodi PCM transition issue remain deferred.

## Dolby Vision FEL

FEL currently supports **7.7.0.2 (PS7702.4965N)** and **7.7.1.7 (PS7717.5741N)**
with their verified kernels. Other supported audio builds continue as audio-only.
FEL requires a compatible player, such as **Kodi FEL** or **VLC FEL**, installed
separately. It uses both hardware decoders and the Dolby Vision composer.
There is no player whitelist: players can be installed before or after the module.
Once the module is running, no extra reboot or player root grant is needed.
Ordinary players do not use FEL automatically; a player must explicitly start a
session and supply decoded enhancement-layer frames. See the [FEL interface](docs/FEL-INTERFACE.md).
FEL playback, audio and seeking have been checked at 24 fps, and at 25 fps over
SMB on PS7717. 4K60 FEL presentation still needs improvement.

v0.2.2 fixes FEL diagnostic flicker while retaining protection against green
flashes during seeking. Tested in Kodi FEL and VLC FEL on both supported builds.

Set `ENABLE_FEL=0` in `config.sh` and reboot to disable FEL while retaining audio fixes. Disabling the module and rebooting removes all in-memory patches.

## Deferred issues

- 4K60 FEL frame pacing and recoverable Kodi seek warnings.
- Alexa interaction during TrueHD and Kodi PCM-to-compressed transitions.

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
| Plezy 2.22.0 | ✅ DTS-HD MA, DTS:X | ✅ DTS-HD MA, DTS:X |
| Wholphin 1.0.8 | ✅ DTS-HD MA, DTS:X | ✅ DTS-HD MA, DTS:X |
| Plex 2026.19.1 | ❌ AAC transcode | ✅ DTS-HD MA, DTS:X |
| Jellyfin Android TV 0.19.10 | ⚠️ DTS core or ❌ AAC transcode | ⚠️ DTS core |

Karat results use RS8182.3811N, Nova 6.4.3, Emby 3.5.63 (`com.mb.android`)
and Kodi 21.3. Gazelle's Plex/Jellyfin results are from PS7702. Kodi uses its own
DTS packer and does not need the DTS fix. Plezy and Wholphin were tested on
Gazelle PS7717 and Karat RS8182 with the combined module; Plezy supplied
already-packed audio, while Wholphin used the patched system packer. App/server transcoding cannot be
reversed by the module.

## Installation

You need root, Magisk and supported firmware. DTS-HD requires a receiver or
TV/eARC chain advertising eight-channel DTS-HD at 192 kHz.

1. Set Fire OS audio to **Best Available** and enable passthrough in your player.
   In Nova, choose **system passthrough**.
2. Install `firetv-av-v0.2.2.zip` in Magisk, then reboot.
3. Allow about one minute after boot before starting playback.


For **Kodi FEL on Cube 3**, add these audio settings to
`/sdcard/Android/data/org.xbmc.kodi.fel/files/.kodi/userdata/advancedsettings.xml`:

```xml
<advancedsettings>
  <audio>
    <superviseaudiodelay>true</superviseaudiodelay>
    <allowmultichannelfloat>true</allowmultichannelfloat>
  </audio>
</advancedsettings>
```

Merge into any existing `<audio>` section and restart Kodi FEL. Delay supervision
helps audio recover after pauses/seeks, sometimes with brief dropouts; multichannel
float enables higher-precision PCM. Recheck passthrough settings after restoring
a Kodi backup.

Use the module ZIP, not GitHub's automatic source ZIP. The installer disables the
old Dolby, DTS and standalone video modules if present. Reboot to switch over, then remove those
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
cat /data/adb/modules/firetv_passthrough/video/fel-status.txt
```

Expect `hdmi_format=6` and DTS status `ACTIVE`. If a stream fails, stop and reopen
it. Repeated audio-service failures stop further DTS attachment; inspect the logs
before retrying. Do not run older trial hooks alongside this module.

The tested DTS profile is 48 kHz with 512-sample core frames. Other source profiles
and receiver hotplug during playback remain untested.

## How it works

The Dolby component maintains HDMI bypass at boot and after wake/restart events,
with brief follow-up checks. The DTS component replaces the system packing path
in memory, preserving HD audio in IEC61937 bursts. On Gazelle, the HAL also retains bypass when Fire OS requests mode 5 during
display changes. Neither audio component polls HDMI while idle. Vendor audio libraries and app APKs are not replaced.

Karat also receives a Magisk overlay correcting the audio utility's exit crash,
generated from the device's own file. No proprietary vendor libraries are bundled; the video helpers are built from
the included kernel-module sources. Frida
runs on-device without ADB or a computer and captures no audio.

## License

Original audio code is MIT licensed; the kernel helpers retain their GPL licenses.
Thanks to **djnice** for the Android FEL player and kernel-helper work adapted for Gazelle. Frida retains its own licenses; see
[NOTICE.md](NOTICE.md). Published module packages must be accompanied by the
Frida source companion archive, which is not a Magisk module.
See [CHANGELOG.md](CHANGELOG.md) for version history.
