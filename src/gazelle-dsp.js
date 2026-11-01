// Correct the voice DSP reference when TDM-B is opened for HDMI playback.
// Resolve APIs by name; enable only for reviewed library identities. No polling,
// HDMI mode changes, microphone suppression or writes to firmware files.
const GAZELLE_DSP_HAL_HASHES = new Set([
  'b60f1b826c74d1c86143be62529f1b0c731cd65df5c140a121410d9e9d52e0cb',
  '18ebffd265df2221663087fbcb8e8a577539700c791eb6bebf43c77b86bd38c9'
]);
const GAZELLE_DSP_ASP_HASH = 'dad091d48856869f6c6c8807b820b71e58a15c3ad37e0b27439160200365aefb';
const GAZELLE_DSP_ALSA_HASH = '8ff05177f1771e485fa5ff198b876c8a040cdd9120e86abe3051cc8836800069';
function gazelleDspFormat(card, device, flags, rate, channels) {
  return card === 0 && device === 1 && (flags & 0x10000000) === 0 &&
    [32000,44100,48000,88200,96000,176400,192000].includes(rate) &&
    [1,2,4,6,8].includes(channels);
}
function correctGazelleDspReference(hal, hooks, enabled, event) {
  try {
    const asp = Process.getModuleByName('libasp.so');
    const alsa = Process.getModuleByName('libamltinyalsa.so');
    const hash = m => Checksum.compute('sha256', File.readAllBytes(m.path));
    if (!GAZELLE_DSP_HAL_HASHES.has(hash(hal)) || hash(asp) !== GAZELLE_DSP_ASP_HASH ||
        hash(alsa) !== GAZELLE_DSP_ALSA_HASH) {
      event('dsp-reference-skipped', {reason:'unreviewed libraries'});
      return;
    }
    const imports = hal.enumerateImports();
    const address = name => {
      const entry = imports.find(x => x.name === name);
      if (!entry || !entry.address.equals(alsa.getExportByName(name)))
        throw new Error('Unexpected ALSA import: ' + name);
      return entry.address;
    };
    const open = address('pcm_open');
    const ready = new NativeFunction(address('pcm_is_ready'), 'int', ['pointer']);
    const command = new NativeFunction(asp.getExportByName('asp_command'), 'int',
      ['int','int','int','pointer','pointer','pointer']);
    hooks.push(Interceptor.attach(open, {
      onEnter(args) {
        this.target = false;
        if (!enabled() || args[3].isNull()) return;
        this.channels = args[3].readU32();
        this.rate = args[3].add(4).readU32();
        this.target = gazelleDspFormat(args[0].toUInt32(), args[1].toUInt32(),
          args[2].toUInt32(), this.rate, this.channels);
      },
      onLeave(result) {
        if (!this.target || !enabled() || result.isNull() || !ready(result)) return;
        const data = Memory.alloc(12);
        data.writeU32(1); // ASP playback reference port: TDM-B
        data.add(4).writeU32(this.rate);
        data.add(8).writeU32(this.channels);
        const rc = command(0, 141, 12, data, ptr(0), ptr(0));
        event('dsp-reference-config', {port:1, rate:this.rate, channels:this.channels, result:rc});
      }
    }));
    event('dsp-reference-installed');
  } catch (e) {
    // A missing optional DSP API must not take down the existing audio patch.
    event('dsp-reference-skipped', {reason:String(e)});
  }
}
