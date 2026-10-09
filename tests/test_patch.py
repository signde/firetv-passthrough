#!/usr/bin/env python3
"""Compare shell patching with the ELF-aware reference; never distribute fixtures."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('reference',root/'scripts/patch_karat_aparam.py')
ref=importlib.util.module_from_spec(spec);spec.loader.exec_module(ref)
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stock',required=True,type=Path)
args=parser.parse_args()
original=args.stock.read_bytes()
expected=ref.patch(original)
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp);source=tmp/'input';output=tmp/'output'
    for data in (original,expected):
        source.write_bytes(data)
        subprocess.run(['sh',str(root/'scripts/patch_karat_aparam.sh'),str(source),str(output)],check=True)
        assert source.read_bytes()==data
        assert output.read_bytes()==expected
    source.write_bytes(b'unsupported')
    result=subprocess.run(['sh',str(root/'scripts/patch_karat_aparam.sh'),str(source),str(output)])
    assert result.returncode!=0
    assert output.read_bytes()==expected
    assert not list(tmp.glob('output.tmp.*'))
print('Stock patch, corrected upgrade, unchanged input, rejection and cleanup passed.')
