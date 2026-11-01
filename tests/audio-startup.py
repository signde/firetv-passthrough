#!/usr/bin/env python3
"""Exercise the real DTS worker across a HAL replacement during attachment."""
from pathlib import Path
import hashlib, os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
for scenario,expected,attempts in [('replace','ACTIVE',2),('same','ATTACH_FAILED',1),('repeated','BLOCKED',3)]:
 with tempfile.TemporaryDirectory() as d:
  base=Path(d);mod=base/'module';mod.mkdir();(mod/'bin').mkdir();proc=base/'proc';proc.mkdir()
  (base/'pid').write_text('100\n');(proc/'100').mkdir();(proc/'100/stat').write_text(' '.join(['0']*21+['1000']))
  (mod/'verify-firmware.sh').write_text('verify_firmware() { AUDIO_SERVICE=test; AUDIO_AGENT=runtime.js; FIRMWARE_PROFILE=test; return 0; }\n')
  (mod/'module.prop').write_text('version=test\n');(mod/'runtime.js').write_text('test')
  utility=base/'aparam';utility.write_text('#!/bin/sh\necho hdmi_format=6\n');utility.chmod(0o755)
  injector=mod/'bin/frida-inject';injector.write_text('''#!/bin/sh
n=$(cat "$TEST_ROOT/count" 2>/dev/null || echo 0); n=$((n+1)); echo "$n" > "$TEST_ROOT/count"
if [ "$SCENARIO" = same ]; then echo 'Unable to attach'; exit 1; fi
if [ "$SCENARIO" = replace ] && [ "$n" -gt 1 ]; then
 echo '{"kind":"ready"}'
 sleep 0.2
 touch "$MODDIR/disable"
 exit 0
fi
old=$(cat "$TEST_ROOT/pid"); new=$((old+1)); echo "$new" > "$TEST_ROOT/pid"
mkdir "$TEST_ROOT/proc/$new"
cp "$TEST_ROOT/proc/$old/stat" "$TEST_ROOT/proc/$new/stat"
rm -rf "$TEST_ROOT/proc/$old"
echo 'Unable to find process'
exit 1
''');injector.chmod(0o755)
  source=(root/'scripts/dts-service.sh').read_text().replace('/dev/firetv_dtshd',str(base/'state')).replace('/proc/',str(proc)+'/').replace('/system/bin/aparam',str(utility))
  (mod/'worker.sh').write_text(source)
  (mod/'payload.sha256').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(mod))+'\n' for p in mod.rglob('*') if p.is_file()))
  bootstrap='''
flock() { return 0; }
getprop() { echo 1; }
pidof() { cat "$TEST_ROOT/pid"; }
sleep() { command sleep 0.01; }
timeout() { shift; "$@"; }
. "$MODDIR/worker.sh"
'''
  result=subprocess.run(['sh','-c',bootstrap],env={**os.environ,'MODDIR':str(mod),'TEST_ROOT':str(base),'SCENARIO':scenario},capture_output=True,text=True,timeout=10)
  status=(mod/'status.txt').read_text()
  assert status.startswith(expected),(scenario,status,result.stderr,(mod/'dts.log').read_text())
  assert int((base/'count').read_text())==attempts
  assert (mod/'blocked').exists()==(scenario=='repeated')
  print('PASS:',scenario,status.strip(),'attempts=',attempts)
