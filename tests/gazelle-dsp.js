const assert=require('node:assert/strict'),vm=require('node:vm'),fs=require('node:fs');
const source=fs.readFileSync(process.argv[2] || require('node:path').join(__dirname,'../src/gazelle-dsp.js'),'utf8');
function fixture({badHash=false,badImport=false}={}) {
 const calls=[],events=[],hooks=[];let enabled=true;
 const P=(value=0,words=null,index=0)=>({value,toUInt32:()=>value,isNull:()=>value===0&&!words,equals:o=>o.value===value,readU32:()=>words[index],writeU32:v=>{words[index]=v},add:n=>P(value+n,words,index+n/4)});
 const open=P(11),ready=P(12),aspfn=P(13);
 const hal={path:'hal',enumerateImports:()=>[{name:'pcm_open',address:badImport?P(99):open},{name:'pcm_is_ready',address:ready}]};
 const asp={path:'asp',getExportByName:()=>aspfn},alsa={path:'alsa',getExportByName:n=>n==='pcm_open'?open:ready};
 const hashes={hal:'b60f1b826c74d1c86143be62529f1b0c731cd65df5c140a121410d9e9d52e0cb',asp:'dad091d48856869f6c6c8807b820b71e58a15c3ad37e0b27439160200365aefb',alsa:'8ff05177f1771e485fa5ff198b876c8a040cdd9120e86abe3051cc8836800069'};
 const ctx={Process:{getModuleByName:n=>n==='libasp.so'?asp:alsa},Checksum:{compute:(_,p)=>badHash?'bad':hashes[p]},File:{readAllBytes:p=>p},Memory:{alloc:n=>P(100,Array(n/4).fill(0))},ptr:P,
 NativeFunction:function(a){return a.value===12?(p=>p.value===200?1:0):((d,c,n,p)=>{calls.push([d,c,n,p.readU32(),p.add(4).readU32(),p.add(8).readU32()]);return 0})},
 Interceptor:{attach:(address,cb)=>{assert.equal(address.value,11);return cb}}};
 vm.createContext(ctx);vm.runInContext(source,ctx);ctx.correctGazelleDspReference(hal,hooks,()=>enabled,(kind,data)=>events.push({kind,data}));
 function run(card,device,flags,rate,channels,result=200,disableAfter=false){const cb=hooks[0],state={};cb.onEnter.call(state,[P(card),P(device),P(flags),P(100,[channels,rate])]);if(disableAfter)enabled=false;cb.onLeave.call(state,P(result));}
 return {calls,events,hooks,run};
}
let t=fixture();t.run(0,1,0,192000,8);assert.deepEqual(t.calls,[[0,141,12,1,192000,8]]);
t.run(0,1,0,48000,2);assert.deepEqual(t.calls[1],[0,141,12,1,48000,2]);
for(const a of [[1,1,0,192000,8],[0,2,0,192000,8],[0,1,0x10000000,192000,8],[0,1,0,12345,8],[0,1,0,192000,3],[0,1,0,192000,8,0],[0,1,0,192000,8,201]])t.run(...a);
assert.equal(t.calls.length,2);t.run(0,1,0,192000,8,200,true);assert.equal(t.calls.length,2);
for(const opt of [{badHash:true},{badImport:true}]){t=fixture(opt);assert.equal(t.hooks.length,0);assert.equal(t.events.at(-1).kind,'dsp-reference-skipped');}
console.log('PASS: DSP transport selection, successful-open guard, disarm, library/import rejection');
