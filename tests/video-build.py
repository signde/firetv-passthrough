#!/usr/bin/env python3
"""Check rejection of stale video source and damaged generated payloads."""
import json
import os
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

# Fire OS 7.7.1.7 has two distinct kernel builds.
selector = r'''
getprop() {
    case "$1" in
        ro.product.device) echo gazelle ;;
        ro.build.fingerprint) echo "Amazon/gazelle/gazelle:9/$BUILD/1:user/release-keys" ;;
    esac
}
uname() { echo "$KERNEL"; }
sha256sum() { echo "$DRIVER  $1"; }
. "$1/video/verify-device.sh"
verify_video_device && echo "$FEL_PROFILE"
'''

driver = '7c59782906328a9a524719ac0df18ea5d6395d901ca035e3ad7cfed7a9025224'
for build, kernel, digest, profile in (
    ('PS7711.5272N', '5.4.125-ab20', driver, 'ps7711'),
    ('PS7711.5272N', '5.4.125-ab25', driver, None),
    ('PS7717.5741N', '5.4.125-ab20', driver, None),
    ('PS7711.5272N', '5.4.125-ab20', 'unknown', None),
    ('PS7712.5371N', '5.4.125-ab26', driver, 'ps7712'),
    ('PS7712.5371N', '5.4.125-ab25', driver, None),
    ('PS7717.5741N', '5.4.125-ab26', driver, None),
    ('PS7712.5371N', '5.4.125-ab26', 'unknown', None),
    ('PS7713.5443N', '5.4.125-ab21', driver, 'ps7713'),
    ('PS7713.5443N', '5.4.125-ab25', driver, None),
    ('PS7717.5741N', '5.4.125-ab21', driver, None),
    ('PS7713.5443N', '5.4.125-ab21', 'unknown', None),
    ('PS7714.5503N', '5.4.125-ab13', driver, 'ps7714_5503'),
    ('PS7714.5503N', '5.4.125-ab21', driver, None),
    ('PS7714.5503N', '5.4.125-ab13', 'unknown', None),
    ('PS7714.5506N', '5.4.125-ab13', driver, None),
    ('PS7714.5507N', '5.4.125-ab13', driver, None),
    ('PS7714.5506N', '5.4.125-ab16', driver, 'ps7714_5506'),
    ('PS7714.5506N', '5.4.125-ab16', 'unknown', None),
    ('PS7714.5503N', '5.4.125-ab16', driver, None),
    ('PS7714.5507N', '5.4.125-ab16', driver, None),
    ('PS7714.5507N', '5.4.125-ab17', driver, 'ps7714_5507'),
    ('PS7714.5507N', '5.4.125-ab17', 'unknown', None),
    ('PS7714.5506N', '5.4.125-ab17', driver, None),
    ('PS7714.5503N', '5.4.125-ab17', driver, None),
    ('PS7715.5585N', '5.4.125-ab25', driver, 'ps7715'),
    ('PS7715.5585N', '5.4.125-ab25', 'unknown', None),
    ('PS7715.5585N', '5.4.125-ab17', driver, None),
    ('PS7716.5665N', '5.4.125-ab25', driver, 'ps7715'),
    ('PS7716.5665N', '5.4.125-ab17', driver, None),
    ('PS7716.5665N', '5.4.125-ab25', 'unknown', None),
    ('PS7716.5664N', '5.4.125-ab25', driver, None),
    ('PS7717.5741N', '5.4.125-ab25', driver, 'ps7717'),
    ('PS7717.5746N', '5.4.125-ab30', driver, 'ps7717_5746'),
    ('PS7717.5746N', '5.4.125-ab25', driver, None),
    ('PS7717.5741N', '5.4.125-ab30', driver, None),
    ('PS7717.5746N', '5.4.125-ab30', 'unknown', None),
    ('PS7718.5826N', '5.4.125-ab35', driver, 'ps7718'),
    ('PS7718.5826N', '5.4.125-ab30', driver, None),
    ('PS7717.5746N', '5.4.125-ab35', driver, None),
    ('PS7718.5826N', '5.4.125-ab35', 'unknown', None),
):
    result = subprocess.run(['sh', '-c', selector, 'sh', str(root)],
                            env={**os.environ, 'BUILD': build, 'KERNEL': kernel, 'DRIVER': digest},
                            text=True, capture_output=True)
    assert (result.returncode == 0) == (profile is not None), (build, kernel)
    if profile:
        assert result.stdout.strip() == profile
print('PASS: FEL profile selection checks the build, kernel and Dolby driver')
