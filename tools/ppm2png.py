#!/usr/bin/env python3
"""Converte PPM (P6) em PNG sem dependencias. Uso: ppm2png.py entrada.ppm saida.png [escala]"""
import struct, sys, zlib
data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
w, h = map(int, parts[1].split())
px = parts[3]
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
rows = []
for y in range(h):
    line = px[y * w * 3:(y + 1) * w * 3]
    if scale > 1:
        line = b"".join(line[i:i + 3] * scale for i in range(0, len(line), 3))
    for _ in range(scale):
        rows.append(b"\0" + line)
raw = b"".join(rows)
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
