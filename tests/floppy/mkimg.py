#!/usr/bin/env python3
"""Gera a imagem de 1,44 MiB do teste: setor de boot + padrao (k*7+3) por setor."""
import sys
boot, out = sys.argv[1], sys.argv[2]
size = int(sys.argv[3]) if len(sys.argv) > 3 else 1474560
b = open(boot, 'rb').read()
assert len(b) == 512 and b[510:] == b'\x55\xaa', 'setor de boot invalido'
img = bytearray(b)
for k in range(1, size // 512):
    img += bytes([(k * 7 + 3) & 0xff]) * 512
open(out, 'wb').write(img)
