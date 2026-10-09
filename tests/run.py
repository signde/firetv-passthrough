#!/usr/bin/env python3
"""Run the existing native boundary tests with ASan/UBSan and synthetic framing."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='firetv-dts-test-') as tmp:
    tmp = Path(tmp)
    shutil.copyfile(root/'src/native.c', tmp/'native.c')
    shutil.copyfile(root/'tests/native_tests.c', tmp/'native_tests.c')
    # Synthetic framing only, not a playable or copyrighted audio sample.
    core = bytearray(2048)
    core[:4] = bytes.fromhex('7ffe8001')
    core[5] = 15 << 2  # 16 blocks, 512 samples
    core[6] = 127
    core[7] = 15 << 4  # core size minus one = 2047
    core[8] = 13 << 2  # 48 kHz
    ext = bytearray(64)
    ext[:4] = bytes.fromhex('64582025')
    ext[7] = 63 >> 3
    ext[8] = (63 & 7) << 5
    fixture = tmp/'synthetic.raw'
    fixture.write_bytes((core+ext)*2)
    binary = tmp/'native-tests'
    subprocess.run(['clang', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(tmp/'native_tests.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(fixture)], check=True)
