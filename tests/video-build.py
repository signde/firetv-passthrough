#!/usr/bin/env python3
"""Check rejection of stale video source and damaged generated payloads."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
stamp = root/'video/build-inputs.json'
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    for name in [*json.loads(stamp.read_text()), 'video/build-inputs.json']:
        dest = temp/name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root/name, dest)
    command = ['python3', str(temp/'video-build-inputs.py'), 'check']
    subprocess.run(command, check=True)
    for name in ('src/fel/ps7702/dvfel_sc2.c', 'video/fel/ps7702/dvfel.ko'):
        path = temp/name
        saved = path.read_bytes()
        path.write_bytes(saved+b'changed')
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode != 0 and 'changed' in result.stderr, name
        path.write_bytes(saved)
print('PASS: stale source and modified generated payload are rejected')
