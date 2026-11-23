// Gazelle RAW TrueHD corrections. The reviewed HALs share the encoder caller,
// presentation-position function and stream layout. Other paths stay unchanged.
const GAZELLE_TRUEHD_HAL_HASHES = new Set([
  'b60f1b826c74d1c86143be62529f1b0c731cd65df5c140a121410d9e9d52e0cb',
  'bfe65dc43af7ab91ba16bb1993835a153f1442574a2cc0356932052f2bc3aa29',
  '18ebffd265df2221663087fbcb8e8a577539700c791eb6bebf43c77b86bd38c9'
]);
const GAZELLE_TRUEHD_API_HASH = '4967489fdbf3b5fba9922d8bff9ae5a984af1e16249f800b91178e6aaacd29ca';
const GAZELLE_TRUEHD_RATES = new Set([44100, 48000, 88200, 96000, 176400, 192000]);
// Decode decimal kHz as integer Hz even when the audio process uses non-default floating-point rounding.
function edidRateHz(khz) {
  const [whole, fraction = ''] = khz.split('.');
  return fraction.length > 3 ? NaN : Number(whole + fraction.padEnd(3, '0'));
}

// The receiver may describe the same MAT capability on several EDID lines.
function mergeTruehdRates(caps, stock) {
  const rows = caps.trim().split(/\r?\n/).filter(line => line.startsWith('MAT,'));
  if (rows.length < 2 || !/^sup_sampling_rates=\d+(?:\|\d+)*$/.test(stock)) return null;
  const parse = line => /^MAT, (\d+) ch, (\d+(?:\.\d+)?(?:\/\d+(?:\.\d+)?)*) kHz, DepVaule (0x[0-9a-f]+)$/i.exec(line.trim());
  const first = parse(rows[0]);
  if (!first) return null;
  const ratesOf = row => row[2].split('/').map(edidRateHz);
  const original = stock.slice('sup_sampling_rates='.length).split('|').map(Number);
  const initial = new Set(ratesOf(first));
  if (initial.size !== original.length || !original.every(rate => initial.has(rate))) return null;
  const merged = new Set(original);
  for (const line of rows.slice(1)) {
    const row = parse(line);
    if (!row || row[1] !== first[1] || Number(row[3]) !== Number(first[3])) continue;
    for (const rate of ratesOf(row)) if (GAZELLE_TRUEHD_RATES.has(rate)) merged.add(rate);
  }
  return merged.size === initial.size ? null : 'sup_sampling_rates=' + [...merged].sort((a,b) => a-b).join('|');
}

function correctGazelleTruehd(hal, hooks, enabled, event) {
  const pending = [];
  try {
    const api = Process.getModuleByName('libms12api_v24.so');
    // Fire OS regenerates its private Dolby encoder payload across boots.
    // No encoder internals are patched: pin the stable HAL and API wrapper
    // whose caller, ABI and structure offsets are used by these hooks.
    const hash = m => Checksum.compute('sha256', File.readAllBytes(m.path));
    if (Process.arch !== 'arm' || !GAZELLE_TRUEHD_HAL_HASHES.has(hash(hal)) || hash(api) !== GAZELLE_TRUEHD_API_HASH)
      throw new Error('Unreviewed TrueHD library combination');
    const position = hal.getExportByName('aml_audio_get_ms12_presentation_position');
    const process = api.getExportByName('dolby_ms12_mat_encoder_process');
    if (!position.equals(hal.base.add(0x3a785)) ||
        !hal.getExportByName('dolby_truehd_bypass_process').equals(hal.base.add(0x49501)))
      throw new Error('Unexpected TrueHD function layout');
    // The HAL loops using the encoder's consumed-byte count. Smaller input
    // batches avoid overflowing its fixed MAT output buffer on sparse streams.
    // Do not change output capacity or claim that unconsumed input was used.
    let cappedLogged = false;
    const positionRatesLogged = new Set();
    pending.push(Interceptor.attach(process, {
      onEnter(args) {
        if (!enabled() || this.returnAddress.compare(hal.base.add(0x49500)) < 0 ||
            this.returnAddress.compare(hal.base.add(0x49900)) >= 0) return;
        const bytes = args[2].toUInt32();
        if (bytes > 768 && bytes < 1048576) {
          args[2] = ptr(768);
          if (!cappedLogged) { cappedLogged = true; event('truehd-input-limited', {bytes, limit:768}); }
        }
      }
    }));
    pending.push(Interceptor.attach(position, {
      onEnter(args) { this.stream = args[0]; this.frames = args[1]; },
      onLeave(result) {
        if (!enabled() || result.toInt32() !== 0 || this.stream.isNull() || this.frames.isNull()) return;
        const stream = this.stream;
        // Both public and internal formats must be RAW TrueHD. Exclude IEC,
        // other codecs and invalid rates. Only correct the uninitialized-MS12
        // branch, which skips the normal bypass position adjustment.
        if (stream.add(0xf8).readU32() !== 0x0e000000 ||
            stream.add(0x1cc).readU32() !== 0x0e000000) return;
        const rate = stream.add(0xfc).readU32();
        if (!GAZELLE_TRUEHD_RATES.has(rate)) return;
        const adev = stream.add(0x128).readPointer();
        if (adev.isNull() || adev.add(0x10c1c).readU32() !== 0) return;
        const frames = this.frames.readU64();
        if (frames.compare(uint64(0)) === 0) return;
        // Same +130 ms position adjustment as the reviewed DD+ bypass branch.
        // Adjust the returned counter only, never the stored stream counter or timestamp.
        const correctionFrames = rate * 130 / 1000;
        this.frames.writeU64(frames.add(correctionFrames));
        if (!positionRatesLogged.has(rate)) {
          positionRatesLogged.add(rate);
          event('truehd-position-corrected', {rate, milliseconds:130, frames:correctionFrames});
        }
      }
    }));
    const capsQuery = hal.getExportByName('get_hdmi_sink_cap_new');
    if (!capsQuery.equals(hal.base.add(0x27381))) throw new Error('Unexpected HDMI capability function layout');
    let lastMerged = '';
    pending.push(Interceptor.attach(capsQuery, {
      onEnter(args) {
        const format = args[1].toUInt32();
        this.rateQuery = enabled() && format === 0x0e000000 && !args[0].isNull() &&
          args[0].readCString().split(';').includes('sup_sampling_rates');
      },
      onLeave(result) {
        if (!this.rateQuery || result.isNull()) return;
        try {
          const merged = mergeTruehdRates(File.readAllText('/sys/class/amhdmitx/amhdmitx0/aud_cap'), result.readCString());
          if (!merged) return;
          // The reviewed function returns an owned 1024-byte buffer. Keep its allocation.
          result.writeUtf8String(merged);
          if (merged !== lastMerged) {
            lastMerged = merged;
            event('truehd-capabilities-merged', {rates:merged});
          }
        } catch (_) { /* A disconnected sink may have no readable EDID. */ }
      }
    }));
    hooks.push(...pending);
    try {
      const caps = File.readAllText('/sys/class/amhdmitx/amhdmitx0/aud_cap');
      const first = /^MAT, \d+ ch, ([0-9./]+) kHz,/m.exec(caps);
      if (first) {
        const baseline = first[1].split('/').map(edidRateHz).join('|');
        const merged = mergeTruehdRates(caps, 'sup_sampling_rates=' + baseline);
        if (merged) event('truehd-profile-refresh', {baseline});
      }
    } catch (_) { /* HDMI may be disconnected at startup. */ }
    event('truehd-corrections-installed', {inputLimit:768, rawPositionMs:130, sampleRates:[...GAZELLE_TRUEHD_RATES]});
  } catch (e) {
    for (const hook of pending.reverse()) { try { hook.detach(); } catch (_) {} }
    event('truehd-corrections-skipped', {reason:String(e)});
  }
}
