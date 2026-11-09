#!/usr/bin/env python3
"""Build the combined Fire TV AV module with a pinned Frida runtime."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent

def sha(data):
    return hashlib.sha256(data).hexdigest()

def require_hash(data, expected, label):
    actual = sha(data)
    if actual != expected:
        raise ValueError(f"{label}: unexpected SHA256 {actual}")

def stage_files(names):
    stage = ROOT / '.build/module'
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    for name in names:
        dest = stage / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, dest)
    for script in sorted(stage.rglob('*.sh')):
        subprocess.run(['sh', '-n', str(script)], check=True)
    return stage

def archive(stage, name):
    dest = ROOT / 'dist' / name
    dest.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(dest, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(stage.rglob('*')):
            if not path.is_file():
                continue
            rel = path.relative_to(stage).as_posix()
            info = zipfile.ZipInfo(rel, (2026, 10, 6, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if rel.endswith('.sh') or rel == 'bin/frida-inject' else 0o644
            info.external_attr = (0o100000 | mode) << 16
            z.writestr(info, path.read_bytes())
    digest = sha(dest.read_bytes())
    dest.with_suffix('.zip.sha256').write_text(f'{digest}  {dest.name}\n')
    print(dest)
    print('SHA256', digest)

import argparse
import json
import lzma
import urllib.request

VERSION = '17.22.2'
RUNTIME_SHA = '6a6d539f09cc2ed2679b8bdea3ce2cb343224adc6887d9fb227b5d1f41bebf07'
ARCHIVE_SHA = 'cb9621771f5922272ef64c51259b904027cb026d756864716fc98455f338d356'
URL = f'https://github.com/frida/frida/releases/download/{VERSION}/frida-inject-{VERSION}-android-arm.xz'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, help='Existing official uncompressed ARM32 frida-inject')
    parser.add_argument('--fetch-runtime', action='store_true', help='Download the pinned official runtime')
    args = parser.parse_args()
    if args.runtime and args.fetch_runtime:
        parser.error('Choose --runtime or --fetch-runtime')
    path = args.runtime or ROOT / '.cache/frida-inject'
    if args.fetch_runtime:
        with urllib.request.urlopen(URL, timeout=60) as response:
            compressed = response.read()
        require_hash(compressed, ARCHIVE_SHA, 'Frida download')
        data = lzma.decompress(compressed)
        require_hash(data, RUNTIME_SHA, 'Frida runtime')
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(data)
    if not path.is_file():
        parser.error('Provide --runtime PATH or use --fetch-runtime once')
    runtime = path.read_bytes()
    require_hash(runtime, RUNTIME_SHA, 'Frida runtime')
    for line in (ROOT/'src/karat-audio-profiles.sha256').read_text().splitlines():
        digest, name = line.split()
        require_hash((ROOT/name).read_bytes(), digest, name)
    names = ['bin/karat-audio-profiles.dex', 'src/KaratAudioProfiles.java', 'src/karat-audio-profiles.sha256', 'tools/build-karat-audio-profiles.py', 'module.prop', 'service.sh', 'customize.sh', 'README.md', 'config.sh',
             'verify-firmware.sh', 'src/native.c',
             'src/agent.js', 'src/karat.js', 'src/karat-mode.js', 'src/karat-capabilities.js', 'src/gazelle-mode.js', 'src/gazelle-dsp.js', 'LICENSE', 'NOTICE.md', 'CHANGELOG.md', 'build.py', 'build-video.sh']
    names += [p.relative_to(ROOT).as_posix() for directory in ('firmware', 'licenses', 'third_party', 'scripts', 'video', 'src/fel')
              for p in sorted((ROOT/directory).rglob('*')) if p.is_file() and 'node_modules' not in p.parts and '__pycache__' not in p.parts and not any(part.startswith('.') for part in p.relative_to(ROOT).parts) and not ('src' in p.relative_to(ROOT).parts and p.suffix == '.ko') and p.suffix not in ('.pyc', '.pyo', '.o', '.cmd', '.mod') and p.name not in ('Module.symvers', 'modules.order', 'dvfel.mod.c', 'gazelle_link.mod.c')]
    for required in ('video/fel/ps7702/dvfel.ko', 'video/fel/ps7717/dvfel.ko'):
        if not (ROOT/required).is_file():
            parser.error('Run build-video.sh first: missing '+required)
    subprocess.run(['python3', str(ROOT/'video-build-inputs.py'), 'check'], check=True)
    names += ['video-build-inputs.py', 'BUILDING.md', 'docs/FEL-INTERFACE.md']
    stage = stage_files(names)
    for pattern, count, script, output in (
        ('ps*.sha256', 4, 'agent.js', 'runtime.js'),
        ('karat-*.sha256', 2, 'karat.js', 'runtime-karat.js'),
    ):
        expected = [{line.split()[1]: line.split()[0] for line in p.read_text().splitlines()}
                    for p in sorted((ROOT/'firmware').glob(pattern))]
        if not expected or any(len(profile) != count for profile in expected):
            raise ValueError(f'Expected complete {count}-library profiles: {pattern}')
        source = 'try {\nconst EXPECTED_FIRMWARE_PROFILES=' + json.dumps(expected) + ';\n'
        source += 'const NATIVE_SOURCE=' + json.dumps((ROOT/'src/native.c').read_text()) + ';\n'
        if script == 'agent.js':
            source += (ROOT/'src/gazelle-mode.js').read_text() + '\n'
            source += (ROOT/'src/gazelle-dsp.js').read_text() + '\n'
        else:
            source += (ROOT/'src/karat-mode.js').read_text() + '\n'
            source += (ROOT/'src/karat-capabilities.js').read_text() + '\n'
        source += (ROOT/'src'/script).read_text()
        source += '\n} catch(e) {console.error("DTS_FATAL "+e.stack); throw e;}\n'
        (stage/output).write_text(source)
        subprocess.run(['node', '--check', str(stage/output)], check=True)
    (stage/'bin').mkdir(exist_ok=True)
    (stage/'bin/frida-inject').write_bytes(runtime)
    # Magisk removes these root files after installation. Hash persistent payloads.
    manifest = ''.join(sha(p.read_bytes())+'  '+p.relative_to(stage).as_posix()+'\n'
                       for p in sorted(stage.rglob('*')) if p.is_file() and p.relative_to(stage).as_posix() not in ('customize.sh', 'README.md', 'config.sh'))
    (stage/'payload.sha256').write_text(manifest)
    properties = dict(line.split('=', 1) for line in (ROOT/'module.prop').read_text().splitlines() if '=' in line)
    version = properties['version'].removesuffix('-experimental')
    archive(stage, f'firetv-av-v{version}.zip')

if __name__ == '__main__':
    main()
