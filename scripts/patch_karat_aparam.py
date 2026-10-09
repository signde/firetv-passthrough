#!/usr/bin/env python3
"""Reproduce the tested dependency-only patch from a user's stock Karat ELF."""
import hashlib
import struct
from pathlib import Path

STOCK = '1ec08b6cdf633a683ace6123991b2b5b440f155804cd24b8081de287ad7e41b7'
PATCHED = '690cfd8c33e3c02d68c7e0d1c51415530907bcf41f1cf8784186ba17c897e4f2'

def patch(data):
    if hashlib.sha256(data).hexdigest() != STOCK:
        raise ValueError('Requires the exact supported STOCK Karat aparam binary')
    if data[:6] != b'\x7fELF\x01\x01':
        raise ValueError('Expected ELF32 little endian')
    phoff = struct.unpack_from('<I', data, 28)[0]
    phsize, phcount = struct.unpack_from('<HH', data, 42)
    segments = [struct.unpack_from('<8I', data, phoff+i*phsize) for i in range(phcount)]
    dynamic = [s for s in segments if s[0] == 2]
    if len(dynamic) != 1:
        raise ValueError('Expected one PT_DYNAMIC segment')
    offset, size = dynamic[0][1], dynamic[0][4]
    entries = [struct.unpack_from('<II', data, p) for p in range(offset, offset+size, 8)]
    strings = next(value for tag, value in entries if tag == 5)
    strfile = next(s[1]+strings-s[2] for s in segments
                   if s[0] == 1 and s[2] <= strings < s[2]+s[4])
    remove = []
    for i, (tag, value) in enumerate(entries):
        if tag == 1:
            start = strfile+value
            name = data[start:data.index(b'\0', start)]
            if name == b'libmediaplayerservice.so':
                remove.append(i)
    if len(remove) != 1:
        raise ValueError('Expected exactly one direct libmediaplayerservice dependency')
    del entries[remove[0]]
    entries.append((0, 0))
    result = bytearray(data)
    result[offset:offset+size] = b''.join(struct.pack('<II', *e) for e in entries)
    if hashlib.sha256(result).hexdigest() != PATCHED:
        raise ValueError('Patched output does not match the hardware-tested binary')
    return bytes(result)

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stock', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_bytes(patch(args.stock.read_bytes()))
