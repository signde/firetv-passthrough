// Persistent experimental Gazelle companion for verified audio profiles. See README.md.
// Only a newly opened raw DTS-HD/48k stream is eligible. Existing streams and
// other codecs retain their original functions/configurations.
if(Process.arch!=='arm' || Process.pointerSize!==4) throw new Error('ARM32 required');
const libraryHashes={};
for(const profile of EXPECTED_FIRMWARE_PROFILES) {
  for(const path of Object.keys(profile)) {
    if(!(path in libraryHashes)) libraryHashes[path]=Checksum.compute('sha256',File.readAllBytes(path));
  }
}
if(!EXPECTED_FIRMWARE_PROFILES.some(profile=>
  Object.entries(profile).every(([path,expected])=>libraryHashes[path]===expected)))
  throw new Error('Unsupported vendor library combination');
const cm=new CModule(NATIVE_SOURCE);
// NativeFunction stores an address, not ownership of its CModule. The packaged
// try-block ends after setup; retain the executable code for the script lifetime.
globalThis.__firetvDtshdNativeModule=cm;
const stateSize=new NativeFunction(cm.dtshd_state_size,'uint',[])();
const reset=new NativeFunction(cm.dtshd_reset,'void',['pointer'],{scheduling:'exclusive'});
const discontinuity=new NativeFunction(cm.dtshd_discontinuity,'void',['pointer'],{scheduling:'exclusive'});
const clear=new NativeFunction(cm.dtshd_clear,'void',['pointer','uint']);
const pack=new NativeFunction(cm.dtshd_process,'int',['pointer','pointer','uint','pointer','uint','pointer','int'],{scheduling:'exclusive'});
const hal=Process.getModuleByName('audio.primary.amlogic.so');
const handles=new Map(), streams=new Map(), opening=new Map(), hooks=[];
let scriptPinned=false;
const pendingCarrier=new Map(), retired=[], timings=new Map();
const timingOffsets=[0x1c,0x58,0x5c,0x78,0x7c,0xd8,0xdc];
const timingValues=[192000,8,192000,8,192000,192000,8];
let armed=false, closing=false, started=0, error=null, sequence=0, trialCount=0;
let totalInput=0,totalOutput=0,totalDriver=0;
function event(kind,extra={}) { console.log(JSON.stringify({kind,time:Date.now(),...extra})); }
function words(p,n) { const r=[]; for(let i=0;i<n;i++) r.push(p.add(i*4).readU32()); return r; }
function hook(name,callbacks) { hooks.push(Interceptor.attach(hal.getExportByName(name),callbacks)); }
function fail(message) { if(!error) {error=message;event('trial-error',{message});} }
function aligned(input,n) {
  if(n<10)return false;
  const h=new Uint8Array(input.readByteArray(10));
  return h[0]===0x7f && h[1]===0xfe && h[2]===0x80 && h[3]===1 &&
    ((h[8]>>2)&15)===13 && ((((h[4]&1)<<6)|(h[5]>>2))+1)===16;
}
function restoreTiming(key,reason) {
  const t=timings.get(key);if(!t)return;
  // Invoked before release, or while the decoder is the active process argument.
  // Never dereference a decoder after its release callback has returned.
  timingOffsets.forEach((o,i)=>t.pointer.add(o).writeU32(t.before[i]));
  timings.delete(key);
  for(const s of handles.values())if(s.decoder===key)s.decoder=null;
  event('timing-restored',{decoder:key,reason});maybeUnpin();
}
function maybeUnpin() {
  if(scriptPinned && !streams.size && !timings.size && !Array.from(handles.values()).some(s=>s.pinned)) {
    scriptPinned=false;Script.unpin();
  }
}
function retireEncoder(key,reason) {
  const s=handles.get(key);if(!s)return;
  handles.delete(key);
  for(const [t,h] of pendingCarrier)if(h===key)pendingCarrier.delete(t);
  // Owner close has completed. Keep two old buffers rooted for diagnostics;
  // playback must not retain a pointer after its owning stream is destroyed.
  retired.push(s);if(retired.length>2)retired.shift();
  if(!handles.size && !streams.size)error=null;
  event('encoder-retired',{handle:key,owner:s.owner,reason});maybeUnpin();
}
const processAddress=hal.getExportByName('aml_spdif_encoder_process');
const original=new NativeFunction(processAddress,'int',['pointer','pointer','uint','pointer','pointer']);
const replacement=new NativeCallback((handle,input,n,outp,outn)=>{
  const s=handles.get(handle.toString());
  if(!s || s.stock)return original(handle,input,n,outp,outn);
  if(error) {outp.writePointer(s.output || ptr(0));outn.writeU32(0);return -1;}
  try {
    if(!s.started) {
      let eligible=aligned(input,n), reason='unsupported input profile or first-write boundary';
      if(eligible) {
        const h=new Uint8Array(input.readByteArray(10));
        const core=(((h[5]&3)<<12)|(h[6]<<4)|(h[7]>>4))+1;
        eligible=core>=10 && n>=core+10 && input.add(core).readU32()===0x25205864;
      }
      if(eligible) {
        reason='receiver DTS-HD/192k capability unavailable';
        let caps='';try {caps=File.readAllText('/sys/class/amhdmitx/amhdmitx0/aud_cap');}catch(e){}
        eligible=caps.split('\n').some(line=>line.startsWith('DTS-HD, 8 ch,') && /[\/,]192 kHz/.test(line));
      }
      if(!eligible) {s.stock=true;event('stock-fallback',{handle:handle.toString(),reason});return original(handle,input,n,outp,outn);}
      s.state=Memory.alloc(stateSize);s.output=Memory.alloc(8*1024*1024);s.capacity=8*1024*1024;s.length=Memory.alloc(4);
      reset(s.state);s.pinned=true;
      if(!scriptPinned){Script.pin();scriptPinned=true;}
    }
    if(s.awaitingCarrier && !s.configured)
      throw new Error('Previous packed buffer did not open a verified HBR carrier');
    if(s.flushPending || s.resumePending) {
      const restart=aligned(input,n);
      if(s.flushPending && !restart)throw new Error('Post-flush input is not an aligned tested DTS unit');
      if(restart) {
        const dropped=s.state.readU32();discontinuity(s.state);s.lastSize=0;
        event('discontinuity',{handle:handle.toString(),reason:s.flushPending?'flush':'resume-aligned',dropped});
      } else event('resume-continuation',{handle:handle.toString()});
      s.flushPending=false;s.resumePending=false;
    }
    // First on-device test is explicitly scoped to the proven 48k/512 sample profile.
    if(!s.started) {
      if(!aligned(input,n))
        throw new Error('First input does not match the tested 48k/512-sample DTS profile');
      s.started=true;started=Date.now();event('trial-started',{handle:handle.toString()});
    }
    s.calls=(s.calls || 0)+1;
    const rc=pack(s.state,input,n,s.output,s.capacity,s.length,0);
    if(rc!==0 && s.calls===1 && !s.configured) {
      s.stock=true;s.pinned=false;maybeUnpin();
      event('stock-fallback',{handle:handle.toString(),reason:'first-buffer packer rejection '+rc});
      return original(handle,input,n,outp,outn);
    }
    if(rc!==0) throw new Error('Native packer returned '+rc);
    const size=s.length.readU32();
    s.lastSize=size;
    if(size) {s.awaitingCarrier=true;pendingCarrier.set(Process.getCurrentThreadId(),handle.toString());}
    if(handle.add(0x14).readU8() && size) clear(s.output,size);
    outp.writePointer(s.output);outn.writeU32(size);
    const id=++sequence;
    totalInput+=n;totalOutput+=size;

    return 0;
  } catch(e) { fail(String(e));outp.writePointer(s.output || ptr(0));outn.writeU32(0);return -1; }
},'int',['pointer','pointer','uint','pointer','pointer']);
Interceptor.replace(processAddress,replacement);

// This stage has fixed 128 KiB buffers. Its caller already loops using the
// reported consumed count, so bound each call rather than enlarging allocations.
const adecs=Process.getModuleByName('libamladecs.so');
const decoderFactory=new NativeFunction(adecs.base.add(0x4195),'pointer',['uint','uint']);
const iecFactory=adecs.getExportByName('aml_iec_func');
hooks.push(Interceptor.attach(adecs.getExportByName('aml_decoder_release'),{
  onEnter(args) {restoreTiming(args[0].toString(),'decoder-release');}
}));
hooks.push(Interceptor.attach(adecs.getExportByName('aml_decoder_process'),{
  onEnter(args) {
    this.target=false;
    for(const s of handles.values()) {
      if(!s.lastSize || args[1].compare(s.output)<0 || args[1].compare(s.output.add(s.lastSize))>=0) continue;
      this.target=true;this.used=args[3];this.decoder=args[0];
      this.requested=Math.min(args[2].toUInt32(),131072);
      if(!decoderFactory(args[0].readU32(),args[0].add(0xc8).readU32()).equals(iecFactory)) {
        fail('Packed output reached a decoder other than IEC passthrough');this.requested=0;
      }
      if(s.decoder!==args[0].toString()) {
        const d=args[0];
        // This firmware may create the decoder before the first encoder write.
        // Match its verified IEC layout before updating the same rate/channel
        // fields produced by the tested 192k/8ch constructor configuration.
        if(!error && d.add(0x44).readU32()===0x0d000000 && d.add(0x64).readU32()===0x0d000000 &&
           d.add(0x48).readU32()===0x0c000000 && d.add(0x68).readU32()===0x0c000000 &&
           d.add(0x50).readU32()===131072 && d.add(0x70).readU32()===131072) {
          const offsets=timingOffsets, values=timingValues;
          const before=offsets.map(o=>d.add(o).readU32());
          if(timings.has(d.toString())) {fail('Decoder already owned by another target');args[2]=ptr(0);this.requested=0;return;}
          timings.set(d.toString(),{pointer:d,before});
          offsets.forEach((o,i)=>d.add(o).writeU32(values[i]));
          s.decoder=d.toString();
          event('timing-ready',{decoder:s.decoder,offsets,before,after:values,existing:true});
        } else {fail('IEC decoder layout did not match tested HBR timing fields');this.requested=0;}
      }
      if(args[1].add(this.requested).compare(s.output.add(s.lastSize))>0) {
        fail('Decoder input exceeds the packed output bounds');this.requested=0;
      }
      args[2]=ptr(this.requested);
      if(!s.chunkingLogged) {event('decoder-chunking',{maximum:131072});s.chunkingLogged=true;}
      break;
    }
    if(!this.target)restoreTiming(args[0].toString(),'non-target-input');
  },
  onLeave(ret) {
    if(!this.target) return;
    const consumed=this.used.readU32();
    if(consumed>this.requested || (this.requested && consumed===0 &&
       this.decoder.add(0x54).readU32()===0 && this.decoder.add(0x74).readU32()===0))
      fail('Vendor IEC stage made no progress or reported an invalid consumed count');
    if(error) {this.used.writeU32(0);ret.replace(-2);}
  }
}));

hook('aml_audio_spdifout_open',{
  onEnter(args) {
    this.target=false;
    if(error || args[1].isNull()) return;
    const c=words(args[1],6);
    event('spdif-open-observed',{config:c});
    // mixer_main_buffer_write packs raw DTS-HD before nonms12_render opens
    // its IEC output. Bind only to the encoder that ran on this same thread.
    const encoder=pendingCarrier.get(this.threadId);
    if(!encoder || !handles.has(encoder) || c[0]!==0x0d000000 || c[1]!==0x0c000000 || ![48000,192000].includes(c[2])) return;
    if(streams.size || opening.size) {fail('More than one target stream requested');return;}
    this.target=true;this.out=args[0];this.thread=this.threadId;
    // Verified ARM32 spdif_config: format, subformat, rate, channels, mask, flags.
    this.config=Memory.alloc(24);Memory.copy(this.config,args[1],24);
    this.config.add(8).writeU32(192000);
    this.config.add(12).writeU32(8);this.config.add(16).writeU32(0x63f);
    this.trialContext={original:c,pcm:null,encoder,codec:null};opening.set(this.thread,this.trialContext);
    args[1]=this.config;
    event('transport-request',{original:c,replacement:words(this.config,6)});
  },
  onLeave(ret) {
    if(!this.target) return;
    opening.delete(this.thread);
    if(ret.toInt32()!==0) {fail('spdifout_open failed '+ret.toInt32());return;}
    const h=this.out.readPointer(), encoder=this.trialContext.encoder;
    const s=handles.get(encoder), pcm=this.trialContext.pcm;
    streams.set(h.toString(),encoder);
    if(!s || !pcm || pcm.channels!==8 || pcm.rate!==192000 || pcm.format!==0 || !pcm.ok || this.trialContext.codec!==8) {
      fail('HBR output configuration was not verified'); return;
    }
    s.configured=true;s.pcm=pcm.handle;
    event('transport-ready',{handle:h.toString(),encoder,pcm,codec:h.add(8).readU32()});
  }
});
hook('halformat_convert_to_spdif',{
  onEnter(args) {this.trialContext=opening.get(this.threadId);this.format=args[0].toUInt32();this.mask=args[1].toUInt32();},
  onLeave(ret) {if(this.trialContext && this.format===0x0c000000) {
    this.trialContext.codec=ret.toInt32();event('hdmi-codec',{format:this.format,mask:this.mask,codec:ret.toInt32()});
  }}
});
hook('aml_spdif_encoder_open',{
  onEnter(args) {this.out=args[0];this.format=args[1].toUInt32();this.caller=this.returnAddress.sub(hal.base).toString();
    this.owner=this.caller==='0x161ab'?this.context.r10:null;},
  onLeave(ret) {
    if(!armed || closing || error || this.format!==0x0c000000 || ret.toInt32()!==0) return;
    if(Array.from(handles.values()).some(s=>s.pinned)) {event('encoder-skipped',{reason:'another patched stream active'});return;}
    // Exact fingerprinted call site in mixer_main_buffer_write. Other paths
    // remain stock until separately validated, even if their codec is DTS-HD.
    if(this.caller!=='0x161ab') {event('encoder-skipped',{caller:this.caller});return;}
    const h=this.out.readPointer().toString();
    if(!this.owner || this.owner.add(0x2c4).readPointer().toString()!==h) {fail('Encoder owner mismatch');return;}
    const s={trial:++trialCount,owner:this.owner.toString(),configured:false,started:false,stock:false};
    handles.set(h,s);
    event('encoder-created',{handle:h,trial:s.trial,owner:s.owner,caller:this.caller});
  }
});
hook('aml_spdif_encoder_close',{
  onEnter(args) {this.h=args[0].toString();},
  onLeave() {retireEncoder(this.h,'encoder-close');}
});
// Stock raw DTS-HD path leaves its encoder-initialized flag unset. Stream
// destruction therefore skips encoder_close. Track the verified owning stream.
hook('adev_close_output_stream_new',{
  onEnter(args) {this.owner=args[1].toString();this.keys=[];
    for(const [h,s] of handles)if(s.owner===this.owner)this.keys.push(h);},
  onLeave() {for(const h of this.keys)retireEncoder(h,'owner-close');}
});
for(const [name,offset] of [['flush',0x18c09],['flush',0x1c8e1],['resume',0x18ad9]]) {
  hooks.push(Interceptor.attach(hal.base.add(offset),{
    onEnter(args) {this.owner=args[0].toString();},
    onLeave(ret) {if(ret.toInt32()!==0)return;
      for(const s of handles.values())if(s.owner===this.owner) {
        if(name==='flush')s.flushPending=true;else s.resumePending=true;
        event('stream-'+name,{owner:this.owner});
      }
    }
  }));
}
hook('aml_audio_spdifout_processs',{
  onEnter(args) {
    const encoder=streams.get(args[0].toString());
    if(encoder && (error || !handles.get(encoder)?.configured)) {
      args[2]=ptr(0);event('output-suppressed',{reason:'Unverified or failed trial carrier'});
    }
  }
});
hook('aml_audio_spdifout_close',{
  onEnter(args) {this.h=args[0].toString();},
  onLeave() {
    const encoder=streams.get(this.h);if(!encoder)return;
    streams.delete(this.h);const s=handles.get(encoder);
    if(s){s.configured=false;s.awaitingCarrier=false;s.pcm=null;}
    event('transport-closed',{handle:this.h,encoder});maybeUnpin();
  }
});
const alsa=Process.getModuleByName('libamltinyalsa.so');
hooks.push(Interceptor.attach(alsa.getExportByName('pcm_open'),{
  onEnter(args) {this.trialContext=opening.get(this.threadId);if(this.trialContext) {
    const c=words(args[3],5);this.info={channels:c[0],rate:c[1],period_size:c[2],period_count:c[3],format:c[4],device:args[1].toUInt32()};
  }},
  onLeave(ret) {if(this.trialContext) {this.info.handle=ret.toString();this.info.ok=!ret.isNull();this.trialContext.pcm=this.info;event('pcm-open',this.info);}}
}));
hooks.push(Interceptor.attach(alsa.getExportByName('pcm_write'),{
  onEnter(args) {
    this.target=false;const h=args[0].toString();
    for(const s of handles.values()) if(s.pcm===h) {
      this.target=true;const n=args[2].toUInt32();totalDriver+=n;

      break;
    }
  },onLeave(ret) {if(this.target && ret.toInt32()!==0) fail('Driver write failed '+ret.toInt32());}
}));
rpc.exports={
  arm() {armed=true;event('armed');},
  disarm() {armed=false;closing=true;event('disarmed');},
  status() {return {armed,started,error,streams:streams.size,encoders:handles.size,timings:timings.size,trialCount,totalInput,totalOutput,totalDriver};},
  dispose() {
    if(streams.size || handles.size || opening.size || timings.size) throw new Error('Stop playback before removing trial hooks');
    armed=false;for(const h of hooks)h.detach();Interceptor.revert(processAddress);Interceptor.flush();
    // Native code/storage remain rooted until script unload.
    event('disposed');
  }
};
// Run after initialization scope has returned and force collection before
// declaring the module ready. This catches native-code ownership regressions.
setTimeout(()=>{
  try {
    gc();
    const state=Memory.alloc(stateSize),out=Memory.alloc(32768),length=Memory.alloc(4),input=Memory.alloc(1);
    reset(state);discontinuity(state);clear(out,32768);
    if(pack(state,input,0,out,32768,length,0)!==0 || length.readU32()!==0 || state.readU32()!==0)
      throw new Error('Native startup self-test failed');
    armed=true;event('ready',{pid:Process.id,arch:Process.arch,version:'0.2.0',capture:false,nativeAfterGc:true});
  } catch(e) {console.error('DTS_FATAL '+e.stack);}
},1000);
