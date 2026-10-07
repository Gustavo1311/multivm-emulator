#!/usr/bin/env python3
"""Tabela RVA -> simbolo a partir de um PE e do seu PDB publico (llvm-pdbutil):
   pdb_syms.py IMAGEM.exe ARQ.pdb > simbolos.txt   (linhas "rva nome", ordenadas)
   pdb_syms.py --lookup simbolos.txt RVA...        (resolve RVAs em hex)"""
import re, struct, subprocess, sys, bisect

if sys.argv[1] == '--lookup':
    tab = [(int(l.split()[0], 16), l.split()[1]) for l in open(sys.argv[2])]
    rvas = [a for a, _ in tab]
    for x in sys.argv[3:]:
        r = int(x, 16)
        i = bisect.bisect_right(rvas, r) - 1
        print('%x = %s+%x' % (r, tab[i][1], r - tab[i][0]) if i >= 0 else '%x = ?' % r)
    sys.exit(0)

d = open(sys.argv[1], 'rb').read()
lf = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, lf + 6)[0]
optsz = struct.unpack_from('<H', d, lf + 20)[0]
so = lf + 24 + optsz
va = [struct.unpack_from('<I', d, so + 40 * i + 12)[0] for i in range(nsec)]
out = subprocess.run(['llvm-pdbutil', 'dump', '--publics', sys.argv[2]], capture_output=True, text=True).stdout
syms = []
name = None
for line in out.splitlines():
    m = re.search(r'S_PUB32 \[size = \d+\] `(.*)`', line)
    if m:
        name = m.group(1)
        continue
    m = re.search(r'addr = (\d+):(\d+)', line)
    if m and name:
        sec, off = int(m.group(1)), int(m.group(2))
        if 1 <= sec <= nsec:
            syms.append((va[sec - 1] + off, name))
        name = None
for a, n in sorted(syms):
    print('%x %s' % (a, n))
