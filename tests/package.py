#!/usr/bin/env python3
"""Check a built ZIP after Magisk removes installer-only root files."""
import hashlib
from pathlib import Path
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1]) as archive:
    files = {name: archive.read(name) for name in archive.namelist()}
assert not any('__pycache__' in name or name.endswith(('.pyc', '.pyo', '.DS_Store')) for name in files)
assert 'skip_mount' not in files, 'Karat requires the Magisk overlay'
assert b'SKIPMOUNT=true' not in files['customize.sh']
assert b'id=firetv_passthrough\n' in files['module.prop']
manifest = dict((name, digest) for digest, name in
                (line.split(maxsplit=1) for line in files['payload.sha256'].decode().splitlines()))
for name in ('README.md', 'customize.sh'):
    assert name in files, f'Missing archive file: {name}'
    assert name not in manifest, f'Magisk removes {name}'
    del files[name]

def valid(payload):
    return all(name in payload and hashlib.sha256(payload[name]).hexdigest() == digest
               for name, digest in manifest.items())

assert valid(files), 'Post-install payload verification failed'
for name in ('service.sh', 'runtime.js', 'bin/frida-inject', 'verify-firmware.sh',
             'runtime-karat.js', 'src/karat.js', 'src/gazelle-dsp.js', 'module.prop',
             'scripts/dolby-service.sh', 'scripts/dts-service.sh',
             'video/service.sh', 'video/fel-service.sh', 'video/build-inputs.json',
             'video/fel/ps7702/dvfel.ko', 'video/fel/ps7717/dvfel.ko',
             'scripts/recovery.sh', 'scripts/legacy.sh', 'scripts/patch_karat_aparam.sh', 'firmware/ps7688.sha256', 'firmware/ps7713.sha256', 'firmware/ps7715.sha256', *[n for n in files if n.startswith('firmware/karat-') and n.endswith('.sha256')]):
    assert name in manifest, f'Critical payload not covered: {name}'
    damaged = dict(files)
    damaged[name] += b'corruption'
    assert not valid(damaged), f'Corruption not detected: {name}'
assert 'third_party/frida/README.md' in manifest, 'Nested documentation should remain covered'
print('PASS: Magisk cleanup survives; modified service, agent and injector fail verification')

assert b'correctGazelleDspReference(hal, hooks' in files['runtime.js']
assert b'correctGazelleDspReference' not in files['runtime-karat.js']

assert b'ENABLE_HDR' not in files['config.sh']
assert b'supervise.sh' not in files['video/service.sh']
for name in files:
    assert not name.startswith(('src/video/', 'video/link/', 'video/sinks/', 'third_party/frida-java-bridge/')), name
    assert name not in ('action.sh', 'uninstall.sh', 'video/classes.dex', 'video/guard-template.js', 'video/supervise.sh'), name
print('PASS: audio/FEL package excludes the HDR controller and display-switching payloads')
