#!/usr/bin/env python3
"""Simbolos publicos de PDBs antigos (MSF 7.00, registros S_PUB32 e S_PUB32_ST),
que o llvm-pdbutil nao consegue ler: pdb_publics.py IMAGEM ARQ.pdb > simbolos.txt"""
import struct, sys

def streams(path):
    d = open(path, 'rb').read()
    bs, _, nblocks, dirsize, _, mapaddr = struct.unpack_from('<IIIIII', d, 32)
    ndirblocks = (dirsize + bs - 1) // bs
    dirblocks = struct.unpack_from('<%dI' % ndirblocks, d, mapaddr * bs)
    dirdata = b''.join(d[b * bs:(b + 1) * bs] for b in dirblocks)[:dirsize]
    n = struct.unpack_from('<I', dirdata, 0)[0]
    sizes = struct.unpack_from('<%dI' % n, dirdata, 4)
    pos = 4 + 4 * n
    out = []
    for sz in sizes:
        if sz == 0xffffffff:
            sz = 0
        nb = (sz + bs - 1) // bs
        blocks = struct.unpack_from('<%dI' % nb, dirdata, pos)
        pos += 4 * nb
        out.append(b''.join(d[b * bs:(b + 1) * bs] for b in blocks)[:sz])
    return out

def sections(pe):
    d = open(pe, 'rb').read()
    lf = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, lf + 6)[0]
    optsz = struct.unpack_from('<H', d, lf + 20)[0]
    so = lf + 24 + optsz
    return [struct.unpack_from('<I', d, so + 40 * i + 12)[0] for i in range(nsec)]

st = streams(sys.argv[2])
va = sections(sys.argv[1])
dbi = st[3]
symrec = struct.unpack_from('<H', dbi, 20)[0]
rec = st[symrec]
syms = []
p = 0
while p + 4 <= len(rec):
    ln, kind = struct.unpack_from('<HH', rec, p)
    body = rec[p + 4:p + 2 + ln]
    if kind == 0x110E:      # S_PUB32: flags, off, seg, nome C
        _, off, seg = struct.unpack_from('<IIH', body, 0)
        name = body[10:].split(b'\0')[0]
    elif kind == 0x1009:    # S_PUB32_ST: flags, off, seg, nome com prefixo de tamanho
        _, off, seg = struct.unpack_from('<IIH', body, 0)
        name = body[11:11 + body[10]]
    else:
        p += 2 + ln
        continue
    if 1 <= seg <= len(va):
        syms.append((va[seg - 1] + off, name.decode('latin-1')))
    p += 2 + ln
for a, n in sorted(syms):
    print('%x %s' % (a, n))
