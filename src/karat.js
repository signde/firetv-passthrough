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
// Karat ARM32 MediaTek path. Exact HAL/SPDIF pairs are checked above.
// Layouts reviewed in the listed RS8145-RS8182 packages. No audio capture.
const cm=new CModule(NATIVE_SOURCE);
globalThis.__firetvDtshdNativeModule=cm;
const size=new NativeFunction(cm.dtshd_state_size,'uint',[])();
const reset=new NativeFunction(cm.dtshd_reset,'void',['pointer'],{scheduling:'exclusive'});
const clear=new NativeFunction(cm.dtshd_clear,'void',['pointer','uint']);
const pack=new NativeFunction(cm.dtshd_process,'int',['pointer','pointer','uint','pointer','uint','pointer','int'],{scheduling:'exclusive'});
const hal=Process.getModuleByName('audio.primary.mt8696.so');
const spdif=Process.getModuleByName('libmtkaudiospdif.so');
const supports=new NativeFunction(hal.getExportByName('_ZN7android16HDMITxController21isSinkSupportedFormatEjjj'),'bool',['uint','uint','uint']);
const owners=new Map(),encoders=new Map(),opening=new Map(),hooks=[];
let seq=0,armed=false;
const CAPACITY=8*1024*1024, BURST=32768;
function event(kind,extra={}) {console.log(JSON.stringify({kind,time:Date.now(),...extra}));}
function words(p,n) {const out=[];for(let i=0;i<n;i++)out.push(p.add(i*4).readU32());return out;}
function fail(s,e) {if(!s.error){s.error=String(e);event('trial-error',{id:s.id,message:s.error});}}
function release(s) {
  if(!s||s.released)return;
  s.released=true;owners.delete(String(s.owner));
  if(s.encoder)encoders.delete(String(s.encoder));
  Script.unpin();
}
function hook(m,n,c) {hooks.push(Interceptor.attach(m.getExportByName(n),c));}
function aligned(b,n) {
  if(n<10||b.isNull())return false;
  const h=new Uint8Array(b.readByteArray(10));
  return b.readU32()===0x0180fe7f && ((h[8]>>2)&15)===13 &&
    ((((h[4]&1)<<6)|(h[5]>>2))+1)===16;
}
hook(hal,'_ZN7android28AudioALSAPlaybackHandlerHDMI4openEv',{
  onEnter(a) {
    this.s=null;
    if(!armed)return;
    const attr=a[0].add(0xc).readPointer();
    if(attr.isNull()||attr.readU32()!==0x0c000000||attr.add(0x40).readU32()!==48000)return;
    // Static vendor method takes format, rate and channel COUNT, not a mask.
    // Query before changing any handler fields, once per newly opened stream.
    if(!supports(0x0c000000,192000,8)) {
      event('stock-fallback',{reason:'sink does not advertise DTS-HD at 192 kHz / 8 channels'});return;
    }
    if(owners.size||opening.has(this.threadId)) {
      event('stock-fallback',{reason:'another patched stream is active'});return;
    }
    const s={id:++seq,owner:a[0],state:Memory.alloc(size),out:Memory.alloc(CAPACITY),outn:Memory.alloc(4),calls:0,input:0,output:0,error:null,needsBoundary:true};
    reset(s.state);this.s=s;owners.set(String(a[0]),s);opening.set(this.threadId,s);Script.pin();
    event('target-open',{id:s.id});
  },
  onLeave(r) {
    const s=this.s;if(!s)return;opening.delete(this.threadId);
    if(r.toInt32()!==0){release(s);event('open-failed',{id:s.id,result:r.toInt32()});return;}
    const h=s.owner;
    // Encoder owner/back-pointer and carrier layout must agree with the reviewed ABI.
    if(!s.encoder||!h.add(0x254).readPointer().equals(s.encoder)||
       !s.encoder.add(0x3c).readPointer().equals(h)||h.add(0x258).readU8()!==1||
       h.add(0x259).readU8()!==1||!s.carrier||h.add(0x1e0).readPointer().isNull())
      fail(s,'HDMI encoder/carrier ownership check failed');
    event('target-opened',{id:s.id,result:r.toInt32(),config:words(h.add(0x1b8),10),verified:!s.error});
  }
});
hook(spdif,'_ZN7android12SPDIFEncoderC2E14audio_format_t',{
  onEnter(a) {this.s=opening.get(this.threadId);this.p=a[0];this.f=a[1].toUInt32();},
  onLeave() {if(this.s&&this.f===0x0c000000){this.s.encoder=this.p;encoders.set(String(this.p),this.s);}}
});
hook(hal,'_ZN7android28AudioALSAPlaybackHandlerBase13openPcmDriverEjj',{
  onEnter(a) {
    this.s=owners.get(String(a[0]));const s=this.s;if(!s)return;const h=a[0];
    h.add(0x18).writeU32(0x63f);h.add(0x4c).writeU32(8);h.add(0x50).writeU32(192000);
    h.add(0x54).writeU32(65536);h.add(0x58).writeU32(1024);
    [8,192000,1024,4,0,4096,0,0,0,0].forEach((x,i)=>h.add(0x1b8+i*4).writeU32(x));
  },
  onLeave() {
    const s=this.s;if(!s)return;
    const h=s.owner,c=words(h.add(0x1b8),5);
    s.carrier=!h.add(0x1e0).readPointer().isNull()&&c[0]===8&&c[1]===192000&&c[4]===0;
    if(!s.carrier)fail(s,'HBR PCM driver did not open with the required format');
  }
});
hook(hal,'_ZNK7android28AudioALSAPlaybackHandlerBase30transferAudioFormatToPcmFormatE14audio_format_t',{
  onEnter(a) {if(owners.has(String(a[0])))a[0].add(0x259).writeU8(1);}
});
const address=spdif.getExportByName('_ZN7android12SPDIFEncoder5writeEPKvj');
const original=new NativeFunction(address,'int',['pointer','pointer','uint']);
const replacement=new NativeCallback((p,b,n)=>{
  const s=encoders.get(String(p));if(!s)return original(p,b,n);
  if(s.error)return -22;if(n===0)return 0;
  try {
    // Carrier selection happens in open(), before the first compressed buffer.
    // Unsupported or corrupt data must fail this stream, never send core bursts
    // over an HBR carrier. Other stream owners continue through stock code.
    if(!s.carrier)throw Error('Unverified HBR carrier');
    if(s.needsBoundary&&!aligned(b,n))throw Error('Expected an aligned 48 kHz / 512-sample DTS core');
    if(!s.calls)s.emit=new NativeFunction(p.readPointer().add(8).readPointer(),'int',['pointer','pointer','uint']);
    const rc=pack(s.state,b,n,s.out,CAPACITY,s.outn,0);
    if(rc)throw Error('Native packer returned '+rc);
    const bytes=s.outn.readU32();if(bytes>CAPACITY||bytes%BURST)throw Error('Unexpected burst period');
    for(let o=0;o<bytes;o+=BURST) {
      const result=s.emit(p,s.out.add(o),BURST);
      if(result!==BURST)throw Error('Driver write returned '+result);
    }
    s.needsBoundary=false;s.calls++;s.input+=n;s.output+=bytes;
    if(s.calls===1)event('transport-ready',{id:s.id,channels:8,rate:192000,burst:BURST});
    return n;
  } catch(e) {fail(s,e);return -22;}
},'int',['pointer','pointer','uint']);
globalThis.__firetvDtshdReplacement=replacement;
Interceptor.replace(address,replacement);
hook(spdif,'_ZN7android12SPDIFEncoder5resetEv',{
  onEnter(a) {
    const s=encoders.get(String(a[0]));if(s){reset(s.state);s.needsBoundary=true;event('reset',{id:s.id});}
  }
});
hook(hal,'_ZN7android28AudioALSAPlaybackHandlerHDMI5closeEv',{
  onEnter(a) {this.s=owners.get(String(a[0]));},
  onLeave() {const s=this.s;if(s){event('closed',{id:s.id,calls:s.calls,input:s.input,output:s.output,error:s.error});release(s);}}
});
rpc.exports={
  status() {return {armed,owners:owners.size,encoders:encoders.size};},
  dispose() {
    if(owners.size||opening.size)throw Error('Stop playback before removing hooks');
    armed=false;for(const h of hooks)h.detach();Interceptor.revert(address);Interceptor.flush();
  }
};
setTimeout(()=>{
  try {
    gc();
    const state=Memory.alloc(size),out=Memory.alloc(BURST),outn=Memory.alloc(4),input=Memory.alloc(1);
    reset(state);clear(out,BURST);
    if(pack(state,input,0,out,BURST,outn,0)!==0||outn.readU32()!==0||state.readU32()!==0)
      throw Error('Native startup self-test failed');
    armed=true;event('ready',{pid:Process.id,arch:Process.arch,version:'0.2.0',device:'karat',capture:false,nativeAfterGc:true});
  } catch(e) {console.error('DTS_FATAL '+e.stack);}
},1000);
