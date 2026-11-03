// Keep the voice DSP reference off TDM-B while its playback clock changes.
// Select the actual transport only on first write; park before open/close.
// Reviewed Gazelle libraries only. No polling or microphone suppression.
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
  const pendingHooks = [];
  try {
    const asp = Process.getModuleByName('libasp.so');
    const alsa = Process.getModuleByName('libamltinyalsa.so');
    const hash = m => Checksum.compute('sha256', File.readAllBytes(m.path));
    if (!GAZELLE_DSP_HAL_HASHES.has(hash(hal)) || hash(asp) !== GAZELLE_DSP_ASP_HASH || hash(alsa) !== GAZELLE_DSP_ALSA_HASH) {
      event('dsp-reference-skipped', {reason:'unreviewed libraries'}); return;
    }
    const imports = hal.enumerateImports();
    const address = name => {
      const entry = imports.find(x => x.name === name);
      if (!entry || !entry.address.equals(alsa.getExportByName(name))) throw new Error('Unexpected ALSA import: ' + name);
      return entry.address;
    };
    // Resolve every required API before installing any hook.
    const api = Object.fromEntries(['pcm_open','pcm_close','pcm_write','pcm_is_ready'].map(n => [n,address(n)]));
    const ready = new NativeFunction(api.pcm_is_ready, 'int', ['pointer']);
    const aspAddress = asp.getExportByName('asp_command');
    const command = new NativeFunction(aspAddress, 'int', ['int','int','int','pointer','pointer','pointer']);
    const handles = new Map(), internal = new Set();
    // Reviewed Gazelle TDM-A dummy/reference configuration. It is independent
    // of the TDM-B HDMI clock; stock HAL also selects it for multichannel PCM.
    let fallback = [0,48000,8], applied = null, owner = null, changing = 0;
    let generation = 0;
    const keyOf = a => a.join('/');
    function payload(v) { const p=Memory.alloc(12);v.forEach((x,i)=>p.add(i*4).writeU32(x));return p; }
    function setReference(v, reason) {
      if (!enabled() || applied === keyOf(v)) return;
      const tid = Process.getCurrentThreadId(), data = payload(v), epoch = generation;
      internal.add(tid);
      let rc;
      try { rc=command(0,141,12,data,ptr(0),ptr(0)); } finally { internal.delete(tid); }
      applied = rc === 0 && epoch === generation ? keyOf(v) : null;
      event('dsp-reference-lifecycle', {reason,port:v[0],rate:v[1],channels:v[2],result:rc,epoch});
    }
    function park(reason) { owner=null;setReference(fallback,reason); }
    pendingHooks.push(Interceptor.attach(aspAddress, {
      onEnter(a) {
        this.record=false;
        if (!enabled() || internal.has(this.threadId) || a[0].toInt32()!==0 || a[1].toInt32()!==141 || a[2].toUInt32()!==12 || a[3].isNull()) return;
        let v=[a[3].readU32(),a[3].add(4).readU32(),a[3].add(8).readU32()];
        const original=v.slice();
        this.fallback = v[0]===0 && [32000,44100,48000,88200,96000,176400,192000].includes(v[1]) && [1,2,4,6,8].includes(v[2]) ? v.slice() : null;
        if (v[0]===1) {
          const h=owner && handles.get(owner);
          v=h && h.playing && !changing ? [1,h.rate,h.channels] : fallback.slice();
          if (keyOf(v)!==keyOf(original)) {this.data=payload(v);a[3]=this.data;event('dsp-reference-stock-adjusted',{original,applied:v});}
        }
        this.record=true;this.value=keyOf(v);this.epoch=generation;
      },
      onLeave(r) {
        if(!this.record)return;
        const current = r.toInt32()===0 && this.epoch===generation;
        if(current && this.fallback) fallback=this.fallback;
        applied=current ? this.value : null;
      }
    }));
    pendingHooks.push(Interceptor.attach(api.pcm_open, {
      onEnter(a) {
        this.target=false;
        if (!enabled() || a[3].isNull()) return;
        this.channels=a[3].readU32();this.rate=a[3].add(4).readU32();
        this.target=gazelleDspFormat(a[0].toUInt32(),a[1].toUInt32(),a[2].toUInt32(),this.rate,this.channels);
        if(this.target) {generation++;changing++;park('before-open');}
      },
      onLeave(r) {
        if(!this.target)return;
        changing--;
        if(!enabled() || r.isNull() || !ready(r))return;
        handles.set(r.toString(),{rate:this.rate,channels:this.channels,playing:false});
        event('dsp-output-open',{pcm:r.toString(),rate:this.rate,channels:this.channels});
      }
    }));
    pendingHooks.push(Interceptor.attach(api.pcm_write, {
      onEnter(a) {
        this.h=null;
        if(!enabled() || a[2].toUInt32()===0)return;
        const key=a[0].toString(),h=handles.get(key);if(!h || changing)return;
        this.h=h;this.key=key;
        h.playing=true;
        if(owner!==key) {generation++;owner=key;}
        setReference([1,h.rate,h.channels],'before-write');
      },
      onLeave(r) {
        if(this.h && r.toInt32()!==0 && handles.get(this.key)===this.h) {
          this.h.playing=false;if(owner===this.key){generation++;park('write-error');}
        }
      }
    }));
    pendingHooks.push(Interceptor.attach(api.pcm_close, {
      onEnter(a) {
        this.key=a[0].toString();this.h=handles.get(this.key);
        if(!this.h)return;
        generation++;changing++;
        if(owner===this.key || !owner)park('before-close');
      },
      onLeave() {
        if(!this.h)return;
        if(handles.get(this.key)===this.h)handles.delete(this.key);
        changing--;
        event('dsp-output-close',{pcm:this.key,played:this.h.playing});
      }
    }));
    hooks.push(...pendingHooks);
    event('dsp-reference-installed',{policy:'playback-lifecycle-v1'});
  } catch(e) {
    for (const hook of pendingHooks.reverse()) { try { hook.detach(); } catch (_) {} }
    event('dsp-reference-skipped',{reason:String(e)});
  }
}
