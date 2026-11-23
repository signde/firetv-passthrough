# Fire TV AV

Fire TV AV is an experimental Magisk module for rooted **Fire TV Cube (3rd gen, Gazelle)**
and **Fire TV Stick 4K Max (2nd gen, Karat)** devices. It lets a compatible receiver decode
Dolby Digital, Dolby Digital Plus (including Atmos), and DTS-HD MA/DTS:X from apps that
send those formats. On supported Cube 3 builds, it also enables hardware Dolby Vision
Profile 7 Full Enhancement Layer (FEL) playback in compatible players.

[Downloads](https://github.com/signde/firetv-passthrough/releases) · [Supported firmware](#supported-firmware) · [Installation](#installation) · [Changelog](CHANGELOG.md)

## Audio passthrough

With **Best Available** selected, Fire OS can render DD/DD+ as Dolby MAT. Fire TV AV
keeps HDMI bypass (mode 6) active so the original DD/DD+ bitstream reaches the receiver.
For DTS-HD, the module preserves the full stream through the system audio packer,
including DTS:X metadata. The player must supply the compressed track. If the server
transcodes it to AAC or the player decodes it to PCM, that is what the module receives.

**Cube 3 TrueHD.** On compatible firmware, the module applies four corrections.

- Applies a 130 ms playback-position correction to Android RAW TrueHD passthrough
  to keep audio in sync. The correction scales with sample rate, including 48, 96
  and 192 kHz and the 44.1 kHz family.
- Limits how much data the vendor TrueHD encoder receives at once to prevent repeated
  packing failures and memory growth on low-bitrate passages.
- Combines the sample rates from matching HDMI MAT entries so apps can see all the
  TrueHD rates the receiver advertises.
- Switches the microphone DSP reference when audio output starts and stops to prevent
  repeated microphone read errors and sustained high CPU usage during audio transitions.

**Stick 4K Max 2.** If Fire OS omits DD or DD+ from its audio capabilities, the module
restores the formats advertised by the connected TV or receiver. It checks Android’s
cached HDMI profiles when it attaches to the audio service and preserves the saved
surround preferences.

## Dolby Vision FEL

On Cube 3, a compatible player supplies both layers of the Profile 7 stream. The Cube
decodes the layers in hardware and combines them in the Dolby Vision composer. Install
[Kodi FEL](https://github.com/signde/xbmc/releases),
[VLC FEL](https://github.com/signde/vlc-fel-android/releases), or
[Wholphin FEL](https://github.com/signde/Wholphin/releases) separately. Other players
can use the same [FEL interface](docs/FEL-INTERFACE.md) if they implement it.

You can install the player before or after the module. Once the module is running,
the player needs neither a root grant nor another reboot. To use audio passthrough
alone, set `ENABLE_FEL=0` in `/data/adb/modules/firetv_passthrough/config.sh` and reboot.

## Supported firmware

**Playback-tested** means playback was verified on hardware. **Static review** means
the audio libraries were checked without a playback test on that build. The installer
checks the device and its audio libraries. On Karat it also checks the audio utility.

### Cube 3 / Gazelle

FEL requires a matching Fire OS build, kernel, and Dolby driver. The TrueHD and
microphone/DSP corrections check the audio libraries separately. Both corrections
have been playback-tested on the builds marked **Yes** in the FEL column. Static
review in the audio column covers basic passthrough only.

| Fire OS | Build(s) | Audio passthrough | Dolby Vision FEL |
| --- | --- | --- | --- |
| 7.6.8.8 | PS7688.4591 | Static review | No |
| 7.6.9.0 | PS7690.4714, PS7690.4716 | Static review | No |
| 7.6.9.6 | PS7696.5226, PS7696.5229 | Static review | No |
| 7.6.9.9 | PS7699.4894, PS7699.4896 | Static review | No |
| 7.7.0.2 | PS7702.4965 | Playback-tested | Yes |
| 7.7.0.4 | PS7704.5024 | Static review | No |
| 7.7.0.6 | PS7706.5106 | Static review | No |
| 7.7.0.7 | PS7707.5376 | Static review | No |
| 7.7.1.0 | PS7710.6003 | Static review | No |
| 7.7.1.1 | PS7711.5272 | Playback-tested | Yes |
| 7.7.1.2 | PS7712.5371 | Playback-tested | Yes |
| 7.7.1.3 | PS7713.5443 | Playback-tested | Yes |
| 7.7.1.4 | PS7714.5503, PS7714.5506, PS7714.5507 | Playback-tested | Yes |
| 7.7.1.5 | PS7715.5585 | Playback-tested | Yes |
| 7.7.1.6 | PS7716.5665 | Playback-tested | Yes |
| 7.7.1.7 | PS7717.5741, PS7717.5746 | Playback-tested | Yes |
| 7.7.1.8 | PS7718.5826 | Playback-tested | Yes |

### Stick 4K Max 2 / Karat

Karat supports audio passthrough only. Build `RS8185.3879N` is unreviewed.

| Fire OS | Build(s) | Audio passthrough |
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
| 8.1.8.2 | RS8182.3811N | Playback-tested |

## Installation

Back up your setup first. Use a rooted device with Magisk and a supported firmware
build. For DTS-HD, the receiver or TV/eARC chain must advertise eight-channel DTS-HD
at 192 kHz.

1. Set Fire OS audio to **Best Available** and enable passthrough in your player.
   In Nova, choose **system passthrough**.
2. Download the `firetv-av-v*.zip` module asset from [Releases](https://github.com/signde/firetv-passthrough/releases).
   Install it in Magisk and reboot. You can install it over previous versions.
3. Wait about a minute after boot before starting playback.

If you use the separate Dolby, DTS, or video modules, the installer disables them.
Once playback works, you can remove them. To go back to those modules, disable Fire
TV AV, re-enable the separate modules, and reboot. To uninstall Fire TV AV, disable
or remove it in Magisk and reboot. Disable it before a firmware update and check
the new build against the firmware table before enabling it again.

### Kodi FEL audio settings

On Cube 3, merge these entries into the `<audio>` section of
`/sdcard/Android/data/org.xbmc.kodi.fel/files/.kodi/userdata/advancedsettings.xml`
and restart Kodi FEL.

```xml
<advancedsettings>
  <audio>
    <superviseaudiodelay>true</superviseaudiodelay>
    <allowmultichannelfloat>true</allowmultichannelfloat>
  </audio>
</advancedsettings>
```

Delay supervision helps audio recover after pauses and seeks, sometimes with brief
dropouts. Multichannel float enables higher-precision PCM. If DD+ drifts with the
Kodi IEC packer, try the Android system (RAW) packer. Start with zero player audio
offset when checking TrueHD sync. Recheck passthrough settings after restoring a Kodi backup.

## App results

DTS output depends on the player and server. These are the results for the tested samples.

| App | Cube 3 / Gazelle | Stick 4K Max 2 / Karat |
| --- | --- | --- |
| Nova, system passthrough | ✅ DTS-HD MA, DTS:X | ✅ DTS-HD MA, DTS:X |
| Emby 3.5.63 | ✅ DTS-HD MA, DTS:X | ✅ DTS-HD MA, DTS:X |
| Kodi 21 | ✅ DTS-HD MA, DTS:X | ✅ DTS-HD MA, DTS:X |
| Plezy 2.22.0 | ✅ DTS-HD MA, DTS:X | ✅ DTS-HD MA, DTS:X |
| Wholphin 1.0.8 | ✅ DTS-HD MA, DTS:X | ✅ DTS-HD MA, DTS:X |
| Plex 2026.19.1 | ❌ AAC transcode | ✅ DTS-HD MA, DTS:X |
| Jellyfin Android TV 0.19.10 | ⚠️ DTS core or ❌ AAC transcode | ⚠️ DTS core |

Karat results are from RS8182.3811N, with Nova 6.4.3, Emby 3.5.63
(`com.mb.android`), and Kodi 21.3. The Cube’s Plex and Jellyfin results are from
PS7702. Kodi and Plezy handle DTS packing themselves. Wholphin used the system packer.
Plezy and Wholphin were tested on Gazelle PS7717 and Karat RS8182.

## Known limitations

- **FEL frame rate and seeking.** Playback, audio, and seeking have been checked at
  24 fps and at 25 fps over SMB on PS7717. 4K60 FEL can drop frames. Kodi may log
  recoverable warnings during seeks. On PS7711.5272N, one Cube rebooted when Kodi
  switched from P7 FEL to HDR10. The kernel logged video canvas warnings. The cause is unknown.
- **Unsupported FEL sample.** `DV FEL All Layers Test (Woman at 80s).mkv` (the rainbow
  pinwheel) uses different picture ordering and GOP structures in its base and enhancement
  layers. This violates [Dolby’s Profile 7 layer-alignment requirement](https://ott.dolby.com/OnDelKits/Dolby_Vision_Online_Delivery_Kit/v1/Documentation/Specs/Visio_Profiles/help_files/topics/c_notes_profile_public.html).
- **Audio transitions.** Invoking Alexa during TrueHD can trigger repeated microphone
  errors. Kodi can also have trouble switching from PCM to compressed output.
- **DTS-HD.** The tested source profile uses 48 kHz audio with 512-sample DTS core
  frames. Other source profiles and receiver hotplug during playback remain untested.
- **Peacock on Karat.** Live streams can intermittently freeze at startup. Reopen
  the stream, or reboot into Fire OS if playback stays frozen.

## Troubleshooting

Run these commands from a root shell after the module has finished starting.

```sh
aparam get 0 hdmi_format
cat /data/adb/modules/firetv_passthrough/status.txt
tail -40 /data/adb/modules/firetv_passthrough/dolby.log
tail -40 /data/adb/modules/firetv_passthrough/dts.log
```

Expect `hdmi_format=6` for HDMI bypass and `ACTIVE` for the DTS component.
On a Cube configured for FEL, the following command should return a status starting with `READY`.

```sh
cat /data/adb/modules/firetv_passthrough/video/fel-status.txt
```

If playback fails, stop and reopen the stream. Repeated audio-service failures
stop further DTS attachment. Check the logs before retrying and run only one set
of audio hooks at a time. When reporting a problem, include the device, exact Fire
OS build, module and player versions, HDMI chain, source format, receiver readout,
and the output above while the problem is happening.

## How it works

The Dolby component restores HDMI bypass after boot, wake, and service restarts.
On both devices, bypass stays active during display refresh-rate changes. The DTS
component packs the HD stream into IEC 61937 bursts in memory. The audio services
respond to startup and playback events without polling HDMI while idle.

On Karat, Magisk overlays a copy of the device’s own audio utility to correct a
crash on exit. The module uses the installed vendor audio libraries and player
APKs. The FEL helpers are built from the included kernel-module sources. Frida
runs on the device without ADB or a computer and captures no audio.

## License

The original audio code is MIT licensed. The kernel helpers retain their GPL
licenses. Thanks to **djnice** for the Android FEL player and kernel-helper work
adapted for Gazelle. Frida retains its own licenses. Distributors must include the
Frida source companion archive alongside the installable module ZIP. [License notices](NOTICE.md)
