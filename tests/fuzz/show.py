import sys, re
w = sys.argv[1]
ref = open(w + "/ref.txt").read().split("\n")
mvm = open(w + "/mvm.txt").read().split("\n")
src = open(w + "/fuzz.S").read()
blocks = re.split(r"# teste (\d+)\n", src)
body = {int(blocks[i]): blocks[i + 1] for i in range(1, len(blocks), 2)}
n = 0
for a, b in zip(ref, mvm):
    if a != b:
        idx = int(a.split()[0], 16)
        lines = body[idx].split("\n")
        # linhas de instrucao: depois do popf, antes do pushf
        s = lines.index([l for l in lines if l.startswith("popf")][0])
        e = lines.index([l for l in lines if l.startswith("pushf")][0])
        fa, fb = a.split(), b.split()
        diffs = [i for i in range(len(fa)) if fa[i] != fb[i]]
        print("%5d: %s   | campos %s" % (idx, " ; ".join(lines[s + 1:e]), diffs))
        n += 1
        if n > int(sys.argv[2] if len(sys.argv) > 2 else 30): break
