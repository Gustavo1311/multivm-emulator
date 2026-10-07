#!/usr/bin/env python3
"""Relatorio do perfil gravado por MVM_PROF=arquivo (mvm-cli): prof_report.py ARQ [N] [BLOCOS]
BLOCOS: mapa gravado por MVM_JIT_BLOCKS=arquivo; atribui o codigo gerado ao RIP do convidado."""
import bisect, collections, subprocess, sys

pcs, maps, in_maps = [], [], False
for line in open(sys.argv[1]):
    if line.startswith('# maps'):
        in_maps = True
    elif in_maps:
        maps.append(line.split())
    elif line.strip():
        pcs.append(int(line, 16))

regions = []  # (inicio, fim, arquivo, deslocamento)
for m in maps:
    if len(m) >= 6 and 'x' in m[1]:
        a, b = (int(v, 16) for v in m[0].split('-'))
        regions.append((a, b, m[5], int(m[2], 16)))

syms = {}
def symbols(path):
    if path not in syms:
        out = subprocess.run(['nm', '-n', '--defined-only', path], capture_output=True, text=True).stdout
        lst = [(int(p[0], 16), p[2]) for p in (l.split() for l in out.splitlines()) if len(p) == 3 and p[1] in 'tTwWiI']
        if not lst:  # bibliotecas sem tabela de simbolos estatica
            out = subprocess.run(['nm', '-D', '-n', '--defined-only', path], capture_output=True, text=True).stdout
            lst = [(int(p[0], 16), p[2]) for p in (l.split() for l in out.splitlines()) if len(p) == 3 and p[1] in 'tTwWiI']
        syms[path] = ([a for a, _ in lst], [n for _, n in lst])
    return syms[path]

blocks = []
if len(sys.argv) > 3:
    for l in open(sys.argv[3], errors='replace'):
        p = l.split()
        blocks.append((int(p[0], 16), int(p[1], 16), int(p[2], 16), int(p[3]), p[4]))
    blocks.sort()
bstarts = [b[0] for b in blocks]
guest = collections.Counter()

count = collections.Counter()
for pc in pcs:
    for a, b, path, off in regions:
        if a <= pc < b:
            addrs, names = symbols(path)
            i = bisect.bisect_right(addrs, pc - a + off) - 1
            name = names[i] if i >= 0 else '?'
            count['%s (%s)' % (name, path.rsplit('/', 1)[-1]) if 'mvm' not in path else name] += 1
            break
    else:
        count['[codigo gerado pelo JIT]'] += 1
        i = bisect.bisect_right(bstarts, pc) - 1
        if i >= 0 and blocks[i][0] <= pc < blocks[i][1]:
            b = blocks[i]
            guest['%x %s (%d insns)' % (b[2], b[4], b[3])] += 1
        elif blocks:
            guest['(bloco desconhecido)'] += 1
total = sum(count.values()) or 1
n = int(sys.argv[2]) if len(sys.argv) > 2 else 30
print('%d amostras' % total)
for k, v in count.most_common(n):
    print('%5.1f%%  %s' % (100.0 * v / total, k))
if guest:
    print('\nBlocos do convidado mais quentes (%% do total):')
    for k, v in guest.most_common(n):
        print('%5.1f%%  %s' % (100.0 * v / total, k))
