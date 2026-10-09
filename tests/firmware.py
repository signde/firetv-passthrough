#!/usr/bin/env python3
"""Check packaged shell and agent guards against an external firmware corpus."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import zipfile

root = Path(__file__).resolve().parents[1]
corpus = Path(sys.argv[1]).resolve()
device = sys.argv[3] if len(sys.argv)>3 else 'gazelle'
assert device in ('gazelle','karat')
sdk = '28' if device=='gazelle' else '30'
runtime = 'runtime.js' if device=='gazelle' else 'runtime-karat.js'
profile_prefix = 'firmware/ps' if device=='gazelle' else 'firmware/karat-'
with zipfile.ZipFile(sys.argv[2]) as z:
    helper = z.read('verify-firmware.sh').decode()
    profiles = {n: z.read(n).decode() for n in z.namelist()
                if n.startswith(profile_prefix) and n.endswith('.sha256')}
    agent = z.read(runtime).decode().split('const cm=new CModule', 1)[0]
    agent += '\n} catch(e) { throw e; }'
expected = {
    'ps7688.4591', 'ps7690.4714', 'ps7690.4716', 'ps7696.5226', 'ps7696.5229',
    'ps7699.4894', 'ps7699.4896', 'ps7702.4965', 'ps7704.5024', 'ps7706.5106',
    'ps7707.5376', 'ps7710.6003', 'ps7711.5272', 'ps7712.5371', 'ps7713.5443',
    'ps7714.5503', 'ps7714.5506', 'ps7714.5507', 'ps7715.5585', 'ps7716.5665',
    'ps7717.5741',
}
if device=='karat':
    expected = {'RS8145.3070N','RS8149.3133N','RS8153.3202N','RS8155.3474N',
                'RS8158.4105N','RS8160.3372N','RS8160.3380N','RS8166.3482N',
                'RS8169.3556N','RS8174.3641N','RS8174.3648N','RS8180.3729N',
                'RS8180.3739N','RS8182.3811N'}
paths = [line.split()[1] for line in next(iter(profiles.values())).splitlines()]
cases = []
for folder in sorted(corpus.iterdir()):
    if not folder.is_dir() or not folder.name.startswith('ps' if device=='gazelle' else 'RS'):
        continue
    buffers = {p: (folder/Path(p).name if device=='gazelle' else folder/p.removeprefix('/vendor/')).read_bytes() for p in paths}
    cases.append((folder.name, buffers, folder.name in expected, 'arm'))
basename='ps7702.4965' if device=='gazelle' else 'RS8182.3811N'
base = next(buffers for name, buffers, ok, arch in cases if name == basename)
assert expected.issubset({name for name,buffers,ok,arch in cases}), 'Corpus missing expected builds'
for path in paths:
    damaged = dict(base)
    damaged[path] += b'corrupt'
    cases.append(('corrupt-' + Path(path).name, damaged, False, 'arm'))
    mixed = dict(base)
    if device=='gazelle':
        mixed[path] = (corpus/'ps7299.3051'/Path(path).name).read_bytes()
    else:
        mixed[path] = b'unknown-vendor-library'
    cases.append(('mixed-' + Path(path).name, mixed, False, 'arm'))
cases.append(('wrong-arch', base, False, 'arm64'))
with tempfile.TemporaryDirectory(prefix='dtshd-firmware-test-') as tmp:
    tmp = Path(tmp)
    (tmp/'firmware').mkdir()
    (tmp/'verify-firmware.sh').write_text(helper)
    node_cases = []
    for name, buffers, expected_ok, arch in cases:
        folder = tmp/name
        folder.mkdir()
        mapping = {}
        for path, content in buffers.items():
            local = folder/Path(path).name
            local.write_bytes(content)
            mapping[path] = str(local)
        for profile, content in profiles.items():
            translated = ''.join(digest+'  '+mapping[path]+'\n'
                                 for digest, path in (line.split() for line in content.splitlines()))
            (tmp/profile).write_text(translated)
        if arch == 'arm':
            command = 'getprop() { case "$1" in ro.product.device) echo "$TEST_DEVICE";; ro.build.version.sdk) echo "$TEST_SDK";; esac; }; . "$1/verify-firmware.sh"; verify_firmware "$1"'
            import os
            env={**os.environ,'TEST_DEVICE':device,'TEST_SDK':sdk}
            result = subprocess.run(['sh', '-c', command, 'sh', str(tmp)],env=env)
            if expected_ok:
                for wrong_device,wrong_sdk in [('unknown',sdk),(device,'999')]:
                    rejected=subprocess.run(['sh','-c',command,'sh',str(tmp)],env={**env,'TEST_DEVICE':wrong_device,'TEST_SDK':wrong_sdk})
                    assert rejected.returncode!=0, ('platform guard',name)
            assert (result.returncode == 0) == expected_ok, ('shell', name)
        node_cases.append(dict(name=name, mapping=mapping, expected=expected_ok, arch=arch))
    (tmp/'cases.json').write_text(json.dumps(node_cases))
    (tmp/'guard.js').write_text(agent)
    js = r"""
const fs=require('fs'), vm=require('vm'), crypto=require('crypto');
const cases=JSON.parse(fs.readFileSync(process.argv[1]));
const source=fs.readFileSync(process.argv[2],'utf8');
for(const c of cases) {
  let accepted=true;
  try {
    vm.runInNewContext(source, {
      Process:{arch:c.arch,pointerSize:c.arch==='arm'?4:8},
      File:{readAllBytes:path=>fs.readFileSync(c.mapping[path])},
      Checksum:{compute:(algorithm,bytes)=>crypto.createHash(algorithm).update(bytes).digest('hex')},
    });
  } catch(e) {accepted=false;}
  if(accepted!==c.expected) throw Error('agent: '+c.name);
}
console.log('PASS: agent firmware profiles and wrong-architecture rejection');
"""
    subprocess.run(['node', '-e', js, str(tmp/'cases.json'), str(tmp/'guard.js')], check=True)
print(f'PASS: {device}: {len(expected)} allowed builds; older/unknown, corrupt, mixed, wrong-platform and wrong-architecture checks passed')
