const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const source = fs.readFileSync(process.argv[2] || require('node:path').join(__dirname, '../src/gazelle-dsp.js'), 'utf8');
function fixture(options = {}) {
  const calls = [], events = [], attached = new Map(), hooks = [], detached = [];
  let enabled = true, result = 0;
  function P(value = 0, words = null, index = 0) {
    return {value, toInt32: () => value, toUInt32: () => value >>> 0,
      toString: () => String(value), isNull: () => value === 0 && !words,
      equals: other => other.value === value, readU32: () => words[index],
      writeU32: v => { words[index] = v; }, add: n => P(value+n, words, index+n/4)};
  }
  const names = ['pcm_open', 'pcm_close', 'pcm_write', 'pcm_is_ready'];
  const api = Object.fromEntries(names.map((n,i) => [n, P(i+10)]));
  const hal = {path: 'hal', enumerateImports: () => names.map(name => ({name,
    address: options.badImport && name === 'pcm_write' ? P(99) : api[name]}))};
  const asp = {path: 'asp', getExportByName: () => P(50)};
  const alsa = {path: 'alsa', getExportByName: n => api[n]};
  const hashes = {
    hal: 'b60f1b826c74d1c86143be62529f1b0c731cd65df5c140a121410d9e9d52e0cb',
    asp: 'dad091d48856869f6c6c8807b820b71e58a15c3ad37e0b27439160200365aefb',
    alsa: '8ff05177f1771e485fa5ff198b876c8a040cdd9120e86abe3051cc8836800069'};
  const ctx = {
    Process: {getModuleByName: n => n === 'libasp.so' ? asp : alsa, getCurrentThreadId: () => 1},
    Checksum: {compute: (_,p) => options.badHash ? 'bad' : hashes[p]},
    File: {readAllBytes: p => p}, Memory: {alloc: n => P(100,Array(n/4).fill(0))}, ptr: P,
    NativeFunction: function(a) {
      if (a.value === api.pcm_is_ready.value) return () => options.notReady ? 0 : 1;
      return (d,c,n,p) => {
        calls.push([p.readU32(),p.add(4).readU32(),p.add(8).readU32()]);
        return result;
      };
    },
    Interceptor: {attach: (a,cb) => {
      if (options.attachFailure && a.value === api.pcm_write.value) throw Error('attach failed');
      attached.set(a.value,cb);
      return {detach: () => {detached.push(a.value); attached.delete(a.value);}};
    }}
  };
  vm.createContext(ctx); vm.runInContext(source,ctx);
  ctx.correctGazelleDspReference(hal,hooks,() => enabled,(kind,data) => events.push({kind,data}));
  function enter(name,args) {const cb=attached.get(api[name].value), s={threadId:1}; cb.onEnter?.call(s,args); return () => cb.onLeave?.call(s,P(result));}
  function invoke(name,args,retval=0) {const cb=attached.get(api[name].value),s={threadId:1}; cb.onEnter?.call(s,args);cb.onLeave?.call(s,P(retval));}
  const open = (rate=192000,channels=8,id=200,card=0,device=1,flags=0) => invoke('pcm_open',[P(card),P(device),P(flags),P(100,[channels,rate])],id);
  const write = (id=200,rc=0,bytes=1024) => invoke('pcm_write',[P(id),P(100),P(bytes)],rc);
  const close = (id=200) => invoke('pcm_close',[P(id)]);
  function stock(values,device=0,rc=0) {
    const a=[P(device),P(141),P(12),P(100,values.slice())],s={threadId:2},cb=attached.get(50);
    cb.onEnter.call(s,a); cb.onLeave.call(s,P(rc));
    return [a[3].readU32(),a[3].add(4).readU32(),a[3].add(8).readU32()];
  }
  return {calls,events,hooks,attached,detached,P,enter,open,write,close,stock,
    disable: () => {enabled=false;}, commandResult: v => {result=v;}};
}
let t=fixture();
t.open();t.close();assert.deepEqual(t.calls,[[0,48000,8]],'unused open must never select TDM-B192');
t.open();t.write();t.write();assert.deepEqual(t.calls.at(-1),[1,192000,8]);assert.equal(t.calls.length,2,'steady writes deduplicated');
assert.deepEqual(t.stock([1,48000,2]),[1,192000,8],'stock request must match active transport');
assert.deepEqual(t.stock([1,48000,2],1),[1,48000,2],'other DSP device untouched');
t.close();assert.deepEqual(t.calls.at(-1),[0,48000,8],'park before active teardown');
t.open(48000,2,201);t.write(201);assert.deepEqual(t.calls.at(-1),[1,48000,2]);t.close(201);
t.open(192000,8,202);t.write(202,-5);assert.deepEqual(t.calls.at(-1),[0,48000,8],'write failure parks reference');
t.write(202);assert.deepEqual(t.calls.at(-1),[1,192000,8]);t.close(202);
// A delayed old close must not erase a newly opened handle at the same address.
t.open(48000,2,203);t.write(203);
const finish=t.enter('pcm_close',[t.P(203)]);t.open(192000,8,203);finish();t.write(203);
assert.deepEqual(t.calls.at(-1),[1,192000,8]);t.close(203);
const n=t.calls.length;t.disable();t.open();t.write();t.close();assert.equal(t.calls.length,n);
for (const args of [[192000,8,200,1],[192000,8,200,0,2],[192000,8,200,0,1,0x10000000],[12345,8],[192000,3]]) {
 t=fixture();t.open(...args);t.write();t.close();assert.equal(t.calls.length,0,'unrelated/unsupported output untouched');
}
for (const options of [{},{notReady:true}]) {
 t=fixture(options);t.open(192000,8,options.notReady?200:0);t.write();assert.deepEqual(t.calls,[[0,48000,8]]);
}
t=fixture();t.open();t.write(200,0,0);assert.equal(t.calls.length,1,'zero bytes do not start playback');
t.commandResult(-1);t.write();t.write();assert.equal(t.calls.length,3,'failed command must be retried');
for(const options of [{badHash:true},{badImport:true},{attachFailure:true}]) {
 t=fixture(options);assert.equal(t.hooks.length,0);assert.equal(t.attached.size,0);
 assert.equal(t.events.at(-1).kind,'dsp-reference-skipped');
 if(options.attachFailure)assert.equal(t.detached.length,2);
}
console.log('PASS: DSP playback/teardown, transient open, dedupe, guards, failed I/O, pointer reuse and atomic hook installation');
