#!/usr/bin/env python3
"""Imprime "nome_pdb URL" do servidor de simbolos da Microsoft para um PE."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
lf = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, lf + 6)[0]
optsz = struct.unpack_from('<H', d, lf + 20)[0]
opt = lf + 24
magic = struct.unpack_from('<H', d, opt)[0]
dd = opt + (112 if magic == 0x20b else 96)
dbg_rva, dbg_sz = struct.unpack_from('<II', d, dd + 6 * 8)
so = opt + optsz
secs = [struct.unpack_from('<IIII', d, so + 40 * i + 8) for i in range(nsec)]
def r2o(r):
    for vs, va, rs, rp in secs:
        if va <= r < va + max(vs, rs):
            return r - va + rp
for i in range(dbg_sz // 28):
    e = struct.unpack_from('<IIHHIIII', d, r2o(dbg_rva) + 28 * i)
    if e[4] == 2:
        o = e[7]
        g = d[o + 4:o + 20]
        age = struct.unpack_from('<I', d, o + 20)[0]
        name = d[o + 24:d.index(b'\0', o + 24)].decode().split('\\')[-1]
        d1, d2, d3 = struct.unpack_from('<IHH', g, 0)
        gid = '%08X%04X%04X%s%X' % (d1, d2, d3, g[8:].hex().upper(), age)
        print(name, 'https://msdl.microsoft.com/download/symbols/%s/%s/%s' % (name, gid, name))
