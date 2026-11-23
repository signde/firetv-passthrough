// Mode 6 can omit plain AC3/EAC3 from freshly enumerated HDMI profiles.
// Repair only missing replies, using the driver's current sink capabilities.
function karatDolbyCapabilities(query, reply, supports) {
  const formats = [[0x09000000, 'AUDIO_FORMAT_AC3'], [0x0a000000, 'AUDIO_FORMAT_E_AC3']];
  const rates = [32000, 44100, 48000];
  const channels = [[2, 'AUDIO_CHANNEL_OUT_STEREO'], [6, 'AUDIO_CHANNEL_OUT_5POINT1'],
                    [8, 'AUDIO_CHANNEL_OUT_7POINT1']];
  if (query === 'sup_formats' && /^sup_formats=/.test(reply) &&
      /AUDIO_FORMAT_(E_AC3_JOC|DTS|DOLBY_TRUEHD|MAT)(\||$|_)/.test(reply)) {
    const values = reply.slice('sup_formats='.length).split('|');
    for (const [format, name] of formats) {
      if (!values.includes(name) && rates.some(rate => channels.some(([ch]) => supports(format, rate, ch))))
        values.push(name);
    }
    return 'sup_formats=' + values.join('|');
  }
  const match = query.match(/^format=(150994944|167772160);(sup_sampling_rates|sup_channels)$/);
  if (!match || reply !== match[2] + '=') return reply;
  const format = Number(match[1]);
  if (match[2] === 'sup_sampling_rates')
    return reply + rates.filter(rate => channels.some(([ch]) => supports(format, rate, ch))).join('|');
  return reply + channels.filter(([ch]) => rates.some(rate => supports(format, rate, ch)))
    .map(([, name]) => name).join('|');
}

function retainKaratDolbyCapabilities(hal, hooks, enabled, event) {
  const getter = hal.getExportByName('_ZN7android18AudioALSAStreamOut13getParametersERKNS_7String8E');
  // Full getter hashes also guard the stream-kind member at +0x1e0.
  const fingerprints = [
    '91d9d594d0fb0cb5debcf065fa0c98a9924a36e26d64840abdd2975ab3ed0cd1',
    '566eed1542e16d3314da584b9a3aaf69e3141f9bc541bb83e0bcf239a660bb40',
    'c3e4add0fd6c7aff92bfac494615bac1928435879f3612bf1b6a1afc15725dfc',
    '24b5069902913b62f29a1d7dfc9beb514b1f0148870d5bd0e9a9a1d2567271e9',
    '4a80e3a54c0df04f21a1abd5b8776469cadbfb3251757c96f16f559231c71dac',
  ];
  if (!fingerprints.includes(Checksum.compute('sha256', getter.readByteArray(6620))))
    throw new Error('Unsupported Karat audio capability getter');
  const supports = new NativeFunction(hal.getExportByName('_ZN7android16HDMITxController21isSinkSupportedFormatEjjj'),
    'bool', ['uint', 'uint', 'uint']);
  const setString = new NativeFunction(Process.getModuleByName('libutils.so').getExportByName('_ZN7android7String85setToEPKc'),
    'int', ['pointer', 'pointer']);
  const getProperty = new NativeFunction(Process.getModuleByName('libc.so').getExportByName('__system_property_get'),
    'int', ['pointer', 'pointer']);
  const modeKey = Memory.allocUtf8String('persist.vendor.audio.hal.hdmi.format');
  event('dolby-capability-sink', {ac3: !!supports(0x09000000, 48000, 2),
    eac3: !!supports(0x0a000000, 48000, 2)});
  let logged = 0;
  hooks.push(Interceptor.attach(getter, {
    onEnter(args) {
      if (!enabled()) return;
      // Direct HDMI and HDMI tunnel profiles only; leave primary/other routes alone.
      const kind = args[1].add(0x1e0).readU32();
      if (kind !== 3 && kind !== 7) return;
      this.query = args[2].readPointer().readUtf8String();
      if (!/^(sup_formats|format=(150994944|167772160);sup_(sampling_rates|channels))$/.test(this.query)) return;
      const value = Memory.alloc(96);
      getProperty(modeKey, value);
      if (value.readUtf8String() !== '6') return;
      this.destination = args[0];
    },
    onLeave() {
      if (!this.destination || !enabled()) return;
      const original = this.destination.readPointer().readUtf8String();
      const applied = karatDolbyCapabilities(this.query, original, supports);
      if (applied === original) return;
      const text = Memory.allocUtf8String(applied);
      const result = setString(this.destination, text);
      if (logged++ < 16) event('dolby-capabilities', {query: this.query, original, applied, result});
    }
  }));
}
