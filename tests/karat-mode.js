const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
let callback, enabled = true, constructed = 0, destroyed = 0, accepted = true;
const events = [], hooks = [];
const setter = {readByteArray: n => n};
const utils = {getExportByName: n => n};
const context = vm.createContext({
  Process: {pointerSize: 4, getModuleByName: () => utils},
  Checksum: {compute: () => accepted ? 'e7a66e8ccdb23219ffb2542d546c498e1e823d9cc8cd516d0e538acadd896606' : 'bad'},
  Memory: {alloc: n => {assert.equal(n, 4); return {};}, allocUtf8String: text => ({text})},
  NativeFunction: function(name) {
    return name.includes('C1') ? (obj, str) => {obj.text = str.text; constructed++;}
      : obj => {assert(obj.text); destroyed++;};
  },
  Interceptor: {attach: (p, cb) => {assert.equal(p, setter); callback = cb; return {};}}
});
vm.runInContext(fs.readFileSync(__dirname + '/../src/karat-mode.js', 'utf8'), context);
const hal = {getExportByName: () => setter};
accepted = false;
assert.throws(() => context.retainKaratBypass(hal, hooks, () => enabled, () => {}), /Unsupported/);
accepted = true;
context.retainKaratBypass(hal, hooks, () => enabled, (kind, data) => events.push({kind, ...data}));
for (const [text, expected] of [
  ['hdmi_format=5', 'hdmi_format=6'],
  ['spdif_format=4;hdmi_format=5', 'spdif_format=4;hdmi_format=6'],
  ['hdmi_format=5;screen_state=on', 'hdmi_format=6;screen_state=on'],
  ['hdmi_format=0', 'hdmi_format=0'], ['hdmi_format=6', 'hdmi_format=6'],
  ['hdmi_format=50', 'hdmi_format=50'], ['other_hdmi_format=5', 'other_hdmi_format=5'],
  ['screen_state=off', 'screen_state=off'], ['', '']
]) {
  const original = {readPointer: () => ({readUtf8String: () => text})};
  const args = [null, original], call = {};
  callback.onEnter.call(call, args);
  assert.equal(args[1] === original ? text : args[1].text, expected);
  assert.equal(original.readPointer().readUtf8String(), text);
  callback.onLeave.call(call);
}
assert.equal(constructed, 3); assert.equal(destroyed, constructed);
assert.equal(events.length, 3);
enabled = false;
const args = [null, {readPointer() {throw Error('disabled hook must not read arguments');}}];
callback.onEnter.call({}, args); callback.onLeave.call({});
console.log('Karat parameter preservation, fail-closed guard and String8 lifetime checks passed');
