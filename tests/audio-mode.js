const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const context = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(__dirname, '../src/gazelle-mode.js'), 'utf8'), context);
for (const [input, expected] of [
  ['hdmi_format=5', 'hdmi_format=6'],
  ['hdmi_format=5;spdif_format=4', 'hdmi_format=6;spdif_format=4'],
  ['screen_state=on;hdmi_format=5;foo=bar', 'screen_state=on;hdmi_format=6;foo=bar'],
  ['hdmi_format=6', 'hdmi_format=6'],
  ['hdmi_format=0', 'hdmi_format=0'],
  ['hdmi_format=50', 'hdmi_format=50'],
  ['other_hdmi_format=5', 'other_hdmi_format=5'],
  ['screen_state=off', 'screen_state=off'],
  ['', ''],
]) assert.equal(context.bypassParameters(input), expected);
console.log('Audio parameter preservation checks passed');
