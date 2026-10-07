#!/usr/bin/env python3
"""Extrai modulos (.ko) de um initramfs cpio (gzip). Uso: extract_modules.py <initramfs> <dir> nome1 nome2 ..."""
import gzip, os, sys
data = open(sys.argv[1], "rb").read()
if data[:2] == b"\x1f\x8b":
    data = gzip.decompress(data)
want = set(sys.argv[3:])
pos = 0
while pos < len(data):
    h = data[pos:pos + 110]
    if h[:6] not in (b"070701", b"070702"):
        break
    fs = int(h[54:62], 16); ns = int(h[94:102], 16)
    name = data[pos + 110:pos + 110 + ns - 1].decode()
    pos += 110 + ns; pos += (4 - pos % 4) % 4
    body = data[pos:pos + fs]; pos += fs; pos += (4 - pos % 4) % 4
    base = os.path.basename(name)
    if base.endswith(".ko") and base[:-3] in want:
        open(os.path.join(sys.argv[2], base), "wb").write(body)
