// Model the callback lifecycle and failure paths; this does not emulate the HAL.
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const memory=Buffer.alloc(128*1024*1024);let next=4096,pins=0,caps=true,packError=0,driverResult=32768,stockCalls=0;
const hooks=new Map(),events=[];let replacement,timer;
class P {
  constructor(n){this.n=Number(n);}
  add(n){return new P(this.n+n);} equals(p){return this.n===p.n;} isNull(){return this.n===0;}
  toString(){return '0x'+this.n.toString(16);} toUInt32(){return this.n>>>0;} toInt32(){return this.n|0;}
  readU32(){return memory.readUInt32LE(this.n);} writeU32(n){memory.writeUInt32LE(n,this.n);}
  readU8(){return memory[this.n];} writeU8(n){memory[this.n]=n;}
  readPointer(){return new P(this.readU32());} writePointer(p){this.writeU32(p.n);}
  readByteArray(n){return Uint8Array.from(memory.subarray(this.n,this.n+n)).buffer;}
}
const alloc=n=>{const p=new P(next);next+=n+16;assert(next<memory.length);return p;};
const sym={open:'_ZN7android28AudioALSAPlaybackHandlerHDMI4openEv',
 ctor:'_ZN7android12SPDIFEncoderC2E14audio_format_t',
 pcm:'_ZN7android28AudioALSAPlaybackHandlerBase13openPcmDriverEjj',
 format:'_ZNK7android28AudioALSAPlaybackHandlerBase30transferAudioFormatToPcmFormatE14audio_format_t',
 reset:'_ZN7android12SPDIFEncoder5resetEv',close:'_ZN7android28AudioALSAPlaybackHandlerHDMI5closeEv'};
function NativeFunction(p){
 const name=String(p);
 if(name==='dtshd_state_size')return ()=>64;
 if(name==='dtshd_reset')return s=>s.writeU32(0);
 if(name==='dtshd_clear')return ()=>{};
 if(name==='dtshd_process')return (s,b,n,out,cap,outn)=>{outn.writeU32(n?32768:0);return packError;};
 if(name.includes('isSinkSupportedFormat'))return (f,r,c)=>{assert.deepEqual([f,r,c],[0x0c000000,192000,8]);return caps;};
 if(name.includes('SPDIFEncoder5write'))return (p,b,n)=>{stockCalls++;return n;};
 if(name==='0x2a')return ()=>driverResult;
 throw Error('Unexpected function '+name);
}
const context=vm.createContext({Process:{arch:'arm',pointerSize:4,id:404,getModuleByName:()=>({getExportByName:n=>n})},
 EXPECTED_FIRMWARE_PROFILES:[{}],NATIVE_SOURCE:'',File:{},Checksum:{},Memory:{alloc},
 CModule:function(){for(const n of ['state_size','reset','clear','process'])this['dtshd_'+n]='dtshd_'+n;},
 NativeFunction,NativeCallback:function(f){return f;},Interceptor:{attach:(n,c)=>{hooks.set(n,c);return {detach(){}};},replace:(n,f)=>replacement=f,revert(){},flush(){}},
 Script:{pin(){pins++;},unpin(){assert(pins>0);pins--; }},rpc:{exports:{}},
 gc(){},setTimeout:f=>timer=f,console:{log:l=>events.push(JSON.parse(l)),error:l=>{throw Error(l);}},Uint8Array,Date,Map});
vm.runInContext(fs.readFileSync(require('path').join(__dirname,'../src/karat.js'),'utf8'),context);timer();
assert.equal(events.at(-1).kind,'ready');
function enter(n,args){const t={threadId:7};hooks.get(sym[n]).onEnter.call(t,args);return t;}
function leave(n,t,r=0){hooks.get(sym[n]).onLeave?.call(t,new P(r));}
function opened(format=0x0c000000,rate=48000,result=0){
 const h=alloc(0x300),a=alloc(0x100),p=alloc(0x40),v=alloc(32);
 h.add(12).writePointer(a);a.writeU32(format);a.add(64).writeU32(rate);
 const t=enter('open',[h]);
 if(t.s){
  leave('ctor',enter('ctor',[p,new P(format)]));p.add(0x3c).writePointer(h);h.add(0x254).writePointer(p);
  h.add(0x258).writeU8(1);p.writePointer(v);v.add(8).writePointer(new P(42));enter('format',[h]);
  const pc=enter('pcm',[h]);h.add(0x1e0).writePointer(new P(123));leave('pcm',pc);
 }
 leave('open',t,result);return {h,p,t};
}
function closed(s){leave('close',enter('close',[s.h]));}
const input=alloc(16);input.writeU32(0x0180fe7f);input.add(5).writeU8(15<<2);input.add(8).writeU8(13<<2);
for(const [format,rate] of [[0x0b000000,48000],[0x0c000000,96000],[0x0d000000,192000]]){
 const s=opened(format,rate);assert(!s.t.s);assert.equal(replacement(s.p,input,16),16);closed(s);
}
caps=false;assert(!opened().t.s);caps=true;assert.equal(pins,0);
let s=opened();assert.equal(pins,1);assert.equal(s.h.add(0x50).readU32(),192000);
assert.equal(replacement(s.p,input,16),16);assert(!opened().t.s,'second owner must remain stock');
enter('reset',[s.p]);assert.equal(replacement(s.p,input,16),16);closed(s);assert.equal(pins,0);
assert.equal(replacement(s.p,input,16),16,'retired pointer must use stock');
s=opened();const bad=alloc(16);assert.equal(replacement(s.p,bad,16),-22);assert.equal(replacement(s.p,input,16),-22);closed(s);
s=opened();packError=2;assert.equal(replacement(s.p,input,16),-22);packError=0;closed(s);
s=opened();driverResult=-1;assert.equal(replacement(s.p,input,16),-22);driverResult=32768;closed(s);
opened(0x0c000000,48000,-1);assert.equal(pins,0,'failed open must release ownership');
s=opened();assert.throws(()=>context.rpc.exports.dispose());closed(s);context.rpc.exports.dispose();
assert.equal(pins,0);assert.equal(context.rpc.exports.status().owners,0);assert(stockCalls>=4);
console.log('PASS: Karat non-target/capability guards, stream ownership, reset, failure latching and cleanup');
