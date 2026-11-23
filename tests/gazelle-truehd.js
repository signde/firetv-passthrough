const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const source = fs.readFileSync(require('node:path').join(__dirname, '../src/gazelle-truehd.js'), 'utf8');
function fixture(options = {}) {
  const memory = new Map(), attached = new Map(), hooks = [], events = [];
  let enabled = true;
  const U = value => ({value:BigInt(value), compare:v => BigInt(value) === v.value ? 0 : BigInt(value) < v.value ? -1 : 1,
    add:v => U(BigInt(value)+BigInt(v)), toString:() => String(value)});
  const P = value => ({value, isNull:() => value === 0, add:n => P(value+n), compare:p => value-p.value,
    equals:p => value === p.value, toUInt32:() => value >>> 0, toInt32:() => value,
    readU32:() => memory.get(value) ?? 0, readPointer:() => P(memory.get(value) ?? 0),
    readU64:() => U(memory.get(value) ?? 0), writeU64:v => memory.set(value,v.value)});
  const base = 0x100000;
  const hal = {path:'hal',base:P(base),getExportByName:n => P(base + (n === 'get_hdmi_sink_cap_new' ? 0x27381 : n === 'dolby_truehd_bypass_process' ? 0x49501 : options.badOffset ? 0x3a789 : 0x3a785))};
  const api = {path:'api',getExportByName:() => P(0x300000)};
  const hashes = {
    hal:options.newFirmware ? '18ebffd265df2221663087fbcb8e8a577539700c791eb6bebf43c77b86bd38c9' : 'b60f1b826c74d1c86143be62529f1b0c731cd65df5c140a121410d9e9d52e0cb',
    api:'4967489fdbf3b5fba9922d8bff9ae5a984af1e16249f800b91178e6aaacd29ca'
  };
  const ctx = {Process:{arch:'arm',getModuleByName:n => {assert.equal(n,'libms12api_v24.so','private encoder need not be loaded at boot');return api;}},
    File:{readAllBytes:p => p}, Checksum:{compute:(_,p) => options.badHash === p ? 'wrong' : hashes[p]}, ptr:P,uint64:U,
    Interceptor:{attach:(p,cb) => {if(options.failAttach && attached.size)throw Error('attach failure');attached.set(p.value,cb);return {detach:()=>attached.delete(p.value)};}}};
  vm.createContext(ctx); vm.runInContext(source,ctx);
  ctx.correctGazelleTruehd(hal,hooks,()=>enabled,(kind,data)=>events.push({kind,data}));
  function batch(n,caller=base+0x4978d) {
    const args=[P(1),P(2),P(n),P(3),P(4),P(245632),P(6)];
    attached.get(0x300000).onEnter.call({returnAddress:P(caller)},args);
    assert.deepEqual(args.slice(3).map(p=>p.value),[3,4,245632,6],'output and consumption arguments untouched');
    return args[2].value;
  }
  function position({format=0xe000000,internal=format,rate=48000,init=0,result=0,frames=48000n,adev=0x800000}={}) {
    const stream=P(0x500000),out=P(0x700000),ts=P(0x710000);
    memory.set(stream.value+0xf8,format);memory.set(stream.value+0x1cc,internal);memory.set(stream.value+0xfc,rate);
    memory.set(stream.value+0x128,adev);memory.set(adev+0x10c1c,init);memory.set(out.value,frames);memory.set(ts.value,12345);
    const cb=attached.get(base+0x3a785),state={};cb.onEnter.call(state,[stream,out,ts]);cb.onLeave.call(state,P(result));
    assert.equal(memory.get(ts.value),12345,'timestamp is not shifted');
    return memory.get(out.value);
  }
  return {batch,position,hooks,events,attached,disable:()=>{enabled=false;}};
}
for(const newFirmware of [false,true]) {
  const t=fixture({newFirmware});assert.equal(t.hooks.length,3);
  assert.equal(t.batch(3072),768);assert.equal(t.batch(768),768);assert.equal(t.batch(100),100);assert.equal(t.batch(0),0);
  assert.equal(t.batch(3072,0x100000),3072,'other encoder callers unchanged');
  assert.equal(t.batch(3072,0x149900),3072,'caller range excludes next function');
  assert.equal(t.position(),54240n);
  for (const rate of [44100,48000,88200,96000,176400,192000]) {
    const correction = BigInt(rate * 130 / 1000);
    assert.equal(t.position({rate,frames:BigInt(rate)}),BigInt(rate)+correction,'130 ms at each valid TrueHD sample rate');
    assert.equal(t.position({rate,frames:0n}),0n,'zero/reset counter must remain zero');
    assert.equal(t.position({rate,frames:0xffffffffn}),0xffffffffn+correction,'correction must retain 64-bit precision');
  }
  for (const rate of [44100,48000,88200,96000,176400,192000]) {
    const events = t.events.filter(e=>e.kind==='truehd-position-corrected' && e.data.rate===rate);
    assert.equal(events.length,1,'log each rate once per attachment');
    assert.equal(events[0].data.frames,rate*130/1000);
  }
  assert.equal(t.position({frames:0xffffffffn}),0xffffffffn+6240n,'64-bit frame count must not truncate');
  for(const params of [{format:0x0a000000},{format:0x0b000000},{format:1},{format:0x0d000000,internal:0xe000000},{internal:0x0a000000},{rate:0},{rate:32000},{rate:12345},{rate:384000},{init:1},{result:-22},{adev:0}])
    assert.equal(t.position(params),48000n,'unrelated or invalid position untouched');
  t.disable();assert.equal(t.batch(3072),3072);assert.equal(t.position(),48000n);
}
for(const options of [{badHash:'hal'},{badHash:'api'},{badOffset:true},{failAttach:true}]) {
  const t=fixture(options);assert.equal(t.hooks.length,0);assert.equal(t.attached.size,0);
  assert.equal(t.events.at(-1).kind,'truehd-corrections-skipped');
}
console.log('PASS: both profiles, batch boundaries, unrelated callers, RAW/IEC and codec/rate guards, timestamps, 64-bit positions, disable and atomic failure');

const merge = vm.runInNewContext(source + '\nmergeTruehdRates');
const narrow = 'MAT, 8 ch, 48 kHz, DepVaule 0x7';
const wide = 'MAT, 8 ch, 44.1/48/88.2/96/176.4/192 kHz, DepVaule 0x7';
const stock = 'sup_sampling_rates=48000';
assert.equal(merge([narrow,wide].join('\n'), stock),'sup_sampling_rates=44100|48000|88200|96000|176400|192000');
for (const line of [narrow,wide.replace('8 ch','6 ch'),wide.replace('0x7','0x3'),'garbled']) {
  assert.equal(merge([narrow,line].join('\n'),stock),null);
}
assert.equal(merge(wide,stock),null);
assert.equal(merge([narrow,wide].join('\n'),'sup_sampling_rates=192000'),null);
