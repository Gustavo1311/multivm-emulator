#!/usr/bin/env python3
"""Extrai um arquivo de um cpio newc (opcionalmente gzip). Uso: cpio_extract.py <cpio> <nome> <saida>"""
import gzip, sys
data = open(sys.argv[1], "rb").read()
if data[:2] == b"\x1f\x8b":
    data = gzip.decompress(data)
pos = 0
while pos < len(data):
    h = data[pos:pos + 110]
    if h[:6] not in (b"070701", b"070702"):
        break
    fs = int(h[54:62], 16); ns = int(h[94:102], 16)
    name = data[pos + 110:pos + 110 + ns - 1].decode()
    pos += 110 + ns; pos += (4 - pos % 4) % 4
    body = data[pos:pos + fs]; pos += fs; pos += (4 - pos % 4) % 4
    if name == sys.argv[2]:
        open(sys.argv[3], "wb").write(body); print("ok", len(body)); break
    if name == "TRAILER!!!":
        print("nao encontrado"); break
