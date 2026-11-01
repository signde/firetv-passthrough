#!/usr/bin/env python3
"""Prevent packaging stale generated video payloads."""
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
STAMP = ROOT/'video/build-inputs.json'
PAYLOADS = [f'video/fel/{profile}/dvfel.ko' for profile in ('ps7702', 'ps7717')]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def current():
    paths = [ROOT/'build-video.sh', ROOT/'video-build-inputs.py']
    for directory in ('src/fel',):
        for p in (ROOT/directory).rglob('*'):
            if not p.is_file() or 'node_modules' in p.parts or p.name == 'guard-local.ts':
                continue
            if p.name in ('Makefile', 'package.json', 'package-lock.json') or p.suffix in ('.h', '.c', '.java', '.ts') and not p.name.endswith('.mod.c'):
                paths.append(p)
    paths += [ROOT/p for p in PAYLOADS]
    return {str(p.relative_to(ROOT)): digest(p) for p in sorted(paths)}

if __name__ == '__main__':
    values = current()
    if sys.argv[1:] == ['record']:
        STAMP.write_text(json.dumps(values, sort_keys=True, indent=2)+'\n')
        print('Recorded video source/payload hashes')
    elif sys.argv[1:] == ['check']:
        if not STAMP.is_file() or json.loads(STAMP.read_text()) != values:
            raise SystemExit('Video sources/payloads changed: run build-video.sh before packaging')
        print('Verified video source/payload hashes')
    else:
        raise SystemExit('Usage: video-build-inputs.py record|check')
