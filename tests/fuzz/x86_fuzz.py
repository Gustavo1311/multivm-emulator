#!/usr/bin/env python3
"""
Fuzzer diferencial x86: gera N testes (instrucao aleatoria + estado aleatorio),
monta um binario de usuario Linux (referencia: qemu-x86_64/qemu-i386) e um binario
bare-metal (MultiVM) com os mesmos enderecos, e compara as saidas.

Uso: x86_fuzz.py <64|32> <semente> <n> <dir_saida>
"""
import os
import random
import sys

MODE = int(sys.argv[1])
SEED = int(sys.argv[2])
N = int(sys.argv[3])
OUT = sys.argv[4]
rnd = random.Random(SEED)

CF, PF, AF, ZF, SF, OF = 1, 4, 0x10, 0x40, 0x80, 0x800
ALL = CF | PF | AF | ZF | SF | OF
LOGIC = CF | PF | ZF | SF | OF

if MODE == 64:
    R64 = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9", "r10", "r11", "r12", "r13", "r14"]
    R32 = ["eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d"]
    R16 = ["ax", "bx", "cx", "dx", "si", "di", "bp", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w"]
    R8 = ["al", "bl", "cl", "dl", "sil", "dil", "bpl", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b"]
    BASE = "r15"
    SIZES = [(8, "b", R8), (16, "w", R16), (32, "l", R32), (64, "q", R64)]
    FULL = R64
else:
    R32 = ["eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"]
    R16 = ["ax", "bx", "cx", "dx", "si", "di", "bp"]
    R8 = ["al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"]
    BASE = "esp"  # nao usado: memoria via endereco absoluto
    SIZES = [(8, "b", R8), (16, "w", R16), (32, "l", R32)]
    FULL = R32


def val(bits=64):
    k = rnd.random()
    if k < 0.15:
        v = rnd.choice([0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
                        0xffffffff, 0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff])
    elif k < 0.4:
        v = rnd.randrange(0, 256)
    else:
        v = rnd.getrandbits(64)
    return v & ((1 << bits) - 1)


def mem(size_bytes):
    off = rnd.randrange(0, 200)
    if MODE == 64:
        return "%d(%%r15)" % off
    return "buf+%d" % off


def imm(bits):
    v = val(bits)
    if bits == 64:
        v = val(32)
        if v & 0x80000000:
            v -= 1 << 32
        return "$%d" % v
    return "$%d" % v


def pick(regs):
    return "%" + rnd.choice(regs)


def reg_no_high(regs):
    return "%" + rnd.choice([r for r in regs if r not in ("ah", "bh", "ch", "dh")])


def gen():
    """retorna (lista de linhas asm, mascara de flags, prologo extra)"""
    kind = os.environ.get("FUZZ_KIND") or rnd.choice(["alu"] * 6 + ["unary", "shift", "shift", "mul", "div", "bit", "bitscan", "ext",
                                     "cmov", "set", "lea", "xchg", "xadd", "cmpxchg", "bswap",
                                     "flags", "string", "shld", "popcnt", "misc", "sse", "sse", "sse", "mmx"])
    bits, suf, regs = rnd.choice(SIZES)
    pre = []
    if kind == "alu":
        op = rnd.choice(["add", "adc", "sub", "sbb", "and", "or", "xor", "cmp", "test"])
        form = rnd.choice(["rr", "rm", "mr", "ir", "im"])
        m = ALL if op in ("add", "adc", "sub", "sbb", "cmp") else LOGIC
        if op == "test" and form == "rm":
            form = "mr"
        if form == "rr":
            a, b = pick(regs), pick(regs)
            if bits == 8 and (a[1:] in ("ah", "bh", "ch", "dh") or b[1:] in ("ah", "bh", "ch", "dh")) and MODE == 64:
                a, b = "%" + rnd.choice(R8[:4] + ["ah", "bh", "ch", "dh"]), "%" + rnd.choice(R8[:4] + ["ah", "bh", "ch", "dh"])
            return ["%s%s %s, %s" % (op, suf, a, b)], m, pre
        if form == "rm":
            return ["%s%s %s, %s" % (op, suf, mem(bits // 8), reg_no_high(regs))], m, pre
        if form == "mr":
            return ["%s%s %s, %s" % (op, suf, reg_no_high(regs), mem(bits // 8))], m, pre
        if form == "ir":
            return ["%s%s %s, %s" % (op, suf, imm(bits), pick(regs))], m, pre
        return ["%s%s %s, %s" % (op, suf, imm(bits), mem(bits // 8))], m, pre
    if kind == "unary":
        op = rnd.choice(["inc", "dec", "neg", "not"])
        m = {"inc": ALL & ~CF, "dec": ALL & ~CF, "neg": ALL, "not": 0}[op]
        dst = pick(regs) if rnd.random() < 0.6 else mem(bits // 8)
        return ["%s%s %s" % (op, suf, dst)], m, pre
    if kind == "shift":
        op = rnd.choice(["shl", "shr", "sar", "rol", "ror", "rcl", "rcr"])
        cnt = rnd.randrange(1, bits)
        if op in ("rcl", "rcr") and bits < 32:
            cnt = rnd.randrange(1, bits)
        m = CF
        if op in ("shl", "shr", "sar"):
            m |= SF | ZF | PF
        if cnt == 1:
            m |= OF
        dst = pick(regs) if rnd.random() < 0.6 else mem(bits // 8)
        if rnd.random() < 0.5:
            pre = ["movb $%d, %%cl" % cnt]
            if dst in ("%cl", "%ch", "%cx", "%ecx", "%rcx"):
                dst = "%" + ("edx" if bits == 32 else "rdx" if bits == 64 else "dx" if bits == 16 else "dl")
            return ["%s%s %%cl, %s" % (op, suf, dst)], m, pre
        return ["%s%s $%d, %s" % (op, suf, cnt, dst)], m, pre
    if kind == "shld":
        if bits == 8:
            bits, suf, regs = 32, "l", R32
        op = rnd.choice(["shld", "shrd"])
        cnt = rnd.randrange(1, bits)
        m = CF | SF | ZF | PF | (OF if cnt == 1 else 0)
        return ["%s%s $%d, %s, %s" % (op, suf, cnt, pick(regs), pick(regs))], m, pre
    if kind == "mul":
        op = rnd.choice(["mul", "imul", "imul2", "imul3"])
        if op in ("imul2", "imul3") and bits == 8:
            bits, suf, regs = 32, "l", R32
        if op == "imul2":
            return ["imul%s %s, %s" % (suf, pick(regs) if rnd.random() < 0.5 else mem(bits // 8), reg_no_high(regs))], CF | OF, pre
        if op == "imul3":
            return ["imul%s %s, %s, %s" % (suf, imm(bits), pick(regs), reg_no_high(regs))], CF | OF, pre
        return ["%s%s %s" % (op, suf, pick(regs))], CF | OF, pre
    if kind == "div":
        op = rnd.choice(["div", "idiv"])
        acc = {8: "ax", 16: "ax", 32: "eax", 64: "rax"}[bits]
        dreg = {8: "bl", 16: "bx", 32: "ebx", 64: "rbx"}[bits]
        dx = {8: None, 16: "dx", 32: "edx", 64: "rdx"}[bits]
        mask = (1 << bits) - 1
        if op == "div":
            if bits == 8:
                k = rnd.randrange(1, 256)
                pre = ["mov $%d, %%ax" % rnd.randrange(0, k * 256), "movb $%d, %%bl" % k]
            else:
                k = rnd.randrange(1, 1 << min(bits, 31))
                pre = ["mov $%d, %%%s" % (val(min(bits, 32)) & mask, acc), "mov $%d, %%%s" % (rnd.randrange(0, k), dx),
                       "mov $%d, %%%s" % (k, dreg)]
        else:
            if bits == 8:
                pre = ["mov $%d, %%ax" % (rnd.randrange(-100, 100) & 0xffff), "movb $%d, %%bl" % rnd.choice([3, 7, 251, 11])]
            else:
                lim = 30000 if bits == 16 else 100000
                ext = {16: "cwtd", 32: "cltd", 64: "cqto"}[bits]
                a_ = rnd.randrange(-lim, lim)
                dv = rnd.choice([3, 7, -5, 13, -1000])
                pre = ["mov $%d, %%%s" % (a_ & mask if bits < 64 else a_, acc), ext,
                       "mov $%d, %%%s" % (dv & mask if bits < 64 else dv, dreg)]
        return ["%s%s %%%s" % (op, suf, dreg)], 0, pre
    if kind == "bit":
        if bits == 8:
            bits, suf, regs = 32, "l", R32
        op = rnd.choice(["bt", "bts", "btr", "btc"])
        if rnd.random() < 0.5:
            return ["%s%s $%d, %s" % (op, suf, rnd.randrange(0, bits), pick(regs) if rnd.random() < 0.5 else mem(bits // 8))], CF, pre
        src = {16: "%dx", 32: "%edx", 64: "%rdx"}[bits]
        pre = ["mov $%d, %s" % (rnd.randrange(0, 200), src)]
        dst = rnd.choice([r for r in regs if r not in ("dx", "edx", "rdx")])
        if rnd.random() < 0.5:
            return ["%s%s %s, %%%s" % (op, suf, src, dst)], CF, pre
        return ["%s%s %s, %s" % (op, suf, src, "buf" if MODE == 32 else "(%r15)")], CF, pre
    if kind == "bitscan":
        if bits == 8:
            bits, suf, regs = 32, "l", R32
        op = rnd.choice(["bsf", "bsr"])
        src = pick(regs)
        pre = ["or%s $1, %s" % (suf, src)] if rnd.random() < 0.8 else []
        if not pre:
            return ["%s%s %s, %s" % (op, suf, src, pick(regs))], ZF, ["or%s $%d, %s" % (suf, 1 << rnd.randrange(0, 15), src)]
        return ["%s%s %s, %s" % (op, suf, src, pick(regs))], ZF, pre
    if kind == "ext":
        c = rnd.choice(["movzbl", "movzwl", "movsbl", "movswl", "movzbw", "movsbw", "cbtw", "cwtl", "cwtd", "cltd"] +
                       (["movzbq", "movzwq", "movsbq", "movswq", "movslq", "cltq", "cqto"] if MODE == 64 else []))
        if c in ("cbtw", "cwtl", "cwtd", "cltd", "cltq", "cqto"):
            return [c], 0, pre
        srcr = {"b": R8, "w": R16, "l": R32}[c[4]]
        dstr = {"l": R32, "w": R16, "q": R64 if MODE == 64 else R32}[c[5]]
        src = (reg_no_high(srcr) if MODE == 64 else pick(srcr)) if rnd.random() < 0.6 else mem(1)
        return ["%s %s, %s" % (c, src, reg_no_high(dstr))], 0, pre
    if kind == "cmov":
        if bits == 8:
            bits, suf, regs = 32, "l", R32
        cc = rnd.choice(["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"])
        return ["cmov%s%s %s, %s" % (cc, suf, pick(regs) if rnd.random() < 0.6 else mem(bits // 8), pick(regs))], 0, pre
    if kind == "cmpcmov":
        # condicao vinda de uma operacao no mesmo bloco (flags preguicosos conhecidos pelo JIT)
        bits2, suf2, regs2 = rnd.choice([s for s in SIZES if s[0] in (16, 32, 64)])
        op = rnd.choice(["cmp", "sub", "add", "test", "and", "xor", "or"])
        src = rnd.choice([pick(regs2), "$%d" % rnd.randrange(-200, 200)]) if op != "test" else pick(regs2)
        cc = rnd.choice(["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"])
        out = ["%s%s %s, %s" % (op, suf2, src, pick(regs2)), "cmov%s%s %s, %s" % (cc, suf2, pick(regs2), pick(regs2))]
        if rnd.random() < 0.5:
            out.append("set%s %s" % (rnd.choice(["e", "b", "l", "a", "g", "s"]), pick(R8)))
        return out, ALL, pre
    if kind == "alu2":
        # ALU com imediato seguida de outra que sobrescreve os flags (flags da 1a mortos no JIT)
        op = rnd.choice(["add", "sub", "and", "or", "xor"])
        bits2, suf2, regs2 = rnd.choice([s for s in SIZES if s[0] in (32, 64)])
        r = pick(regs2)
        v = rnd.choice([rnd.randrange(0, 4096), -rnd.randrange(1, 4096), rnd.randrange(0, 1 << 24) & ~0xfff,
                        (1 << rnd.randrange(1, 31)) - 1, ((1 << rnd.randrange(1, 31)) - 1) << rnd.randrange(0, 8),
                        0x55555555, 0x0f0f0f0f, -256, 0x7fffffff, -0x80000000, rnd.getrandbits(32)])
        v = ((v + (1 << 31)) & 0xffffffff) - (1 << 31)  # faixa do imediato de 32 bits
        first = "%s%s $%d, %s" % (op, suf2, v, r)
        rr = rnd.random()
        if rr < 0.2:
            first = "%s%s $%d, %s" % (op, suf2, v, "%eax" if bits2 == 32 else "%rax")
        elif rr < 0.55:
            first = "%s%s %s, %s" % (op, suf2, pick(regs2), pick(regs2))
        elif rr < 0.75:
            first = "%s%s %s, %s" % (op, suf2, mem(bits2 // 8), pick(regs2))
        after = rnd.choice(["add%s %s, %s" % (suf2, pick(regs2), pick(regs2)), "cmp%s %s, %s" % (suf2, pick(regs2), pick(regs2)),
                            "test%s %s, %s" % (suf2, pick(regs2), pick(regs2))])
        return [first, after], ALL, pre
    if kind == "set":
        cc = rnd.choice(["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"])
        return ["set%s %s" % (cc, pick(R8) if rnd.random() < 0.6 else mem(1))], 0, pre
    if kind == "lea":
        if bits == 8:
            bits, suf, regs = 32, "l", R32
        full = FULL
        b, i = rnd.choice(full), rnd.choice([r for r in full if r not in ("rsp", "esp")])
        sc = rnd.choice([1, 2, 4, 8])
        disp = rnd.randrange(-100000, 100000)
        return ["lea%s %d(%%%s,%%%s,%d), %s" % (suf, disp, b, i, sc, pick(regs))], 0, pre
    if kind == "xchg":
        return ["xchg%s %s, %s" % (suf, reg_no_high(regs), reg_no_high(regs) if rnd.random() < 0.5 else mem(bits // 8))], 0, pre
    if kind == "xadd":
        return ["xadd%s %s, %s" % (suf, reg_no_high(regs), reg_no_high(regs) if rnd.random() < 0.5 else mem(bits // 8))], ALL, pre
    if kind == "cmpxchg":
        acc = {8: "%al", 16: "%ax", 32: "%eax", 64: "%rax"}[bits]
        m = mem(bits // 8)
        pre = ["mov%s %s, %s" % (suf, m, acc)] if rnd.random() < 0.5 else []
        return ["cmpxchg%s %s, %s" % (suf, reg_no_high(regs), m)], ALL, pre
    if kind == "bswap":
        r = rnd.choice(R32 + (R64 if MODE == 64 else []))
        return ["bswap %%%s" % r], 0, pre
    if kind == "flags" and rnd.random() < 0.3:
        # cadeias de multiprecisao: o carry de uma instrucao e lido pela seguinte (mesmo bloco)
        r1, r2, r3, r4 = pick(regs), pick(regs), pick(regs), pick(regs)
        first = rnd.choice(["add", "sub", "adc", "sbb", "inc", "dec", "and", "neg", "shl1", "mul", "imul1", "cmp", "stc", "clc"])
        second = rnd.choice(["adc", "sbb"])
        if first in ("inc", "dec", "neg"):
            l1 = "%s%s %s" % (first, suf, r1)
        elif first == "shl1":
            l1 = "shl%s $1, %s" % (suf, r1)
        elif first in ("mul", "imul1"):
            if bits < 32:
                bits, suf, regs = 32, "l", R32
                r2, r3, r4 = pick(regs), pick(regs), pick(regs)
            l1 = "%s%s %s" % ("mul" if first == "mul" else "imul", suf, r2)
        elif first in ("stc", "clc"):
            l1 = first
        else:
            l1 = "%s%s %s, %s" % (first, suf, r1, r2)
        src = rnd.choice(["$0", "$%d" % rnd.randrange(0, 100), r4])
        return [l1, "%s%s %s, %s" % (second, suf, src, r3)], ALL, pre
    if kind == "flags" and rnd.random() < 0.6:
        # operacao + leitura de condicao no mesmo bloco (exercita a fusao de condicoes do JIT)
        CC = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"]
        r1, r2 = pick(regs), pick(regs)
        op = rnd.choice(["add", "sub", "cmp", "adc", "sbb", "and", "xor", "test", "inc", "dec", "neg",
                         "shl", "shr", "sar", "shl1", "bt", "imul"])
        ok = list(range(16))
        if op in ("inc", "dec", "neg"):
            line = "%s%s %s" % (op, suf, r1)
        elif op in ("shl", "shr", "sar"):
            line = "%s%s $%d, %s" % (op, suf, rnd.randrange(2, 8), r1)
            ok = [c for c in ok if c >> 1 not in (0, 6, 7)]  # OF indefinido
        elif op == "shl1":
            line = "shl%s $1, %s" % (suf, r1)
        elif op == "bt":
            if bits == 8:
                bits, suf, regs = 32, "l", R32
                r1 = pick(regs)
                r2 = pick(regs)
            line = "bt%s $%d, %s" % (suf, rnd.randrange(0, bits), r1)
            ok = [2, 3]
        elif op == "imul":
            if bits == 8:
                bits, suf, regs = 32, "l", R32
                r1, r2 = pick(regs), pick(regs)
            line = "imul%s %s, %s" % (suf, r1, r2)
            ok = [0, 1, 2, 3]
        else:
            line = "%s%s %s, %s" % (op, suf, r1, r2)
        cc = CC[rnd.choice(ok)]
        dst = "%" + rnd.choice(R8[:4]) if MODE == 32 else "%" + rnd.choice(["al", "bl", "cl", "dl", "r8b", "r9b"])
        if rnd.random() < 0.35:
            # flags mortos: rotacao/deslocamento/bt cujos flags sao sobrescritos pelo add
            rop = rnd.choice(["rol", "ror", "shl", "shr", "sar"])
            cnt = "%cl" if rnd.random() < 0.4 else "$%d" % rnd.randrange(0, 70)
            r3 = pick(regs)
            return ["%s%s %s, %s" % (rop, suf, cnt, r3), "add%s %s, %s" % (suf, r3, r2), "set%s %s" % (cc, dst)], 0, pre
        if rnd.random() < 0.5:
            return [line, "set%s %s" % (cc, dst)], 0, pre
        return [line, "j%s 1f" % cc, "not%s %s" % (suf, r2), "1:"], 0, pre
    if kind == "flags":
        c = rnd.choice(["stc", "clc", "cmc", "lahf", "sahf", "pushf_pop"])
        if c == "pushf_pop":
            p = "q" if MODE == 64 else "l"
            return ["push%s $%d" % (p, rnd.getrandbits(12) & 0xcd5), "popf%s" % p], ALL, pre
        return [c], ALL, pre
    if kind == "popcnt":
        if rnd.random() < 0.4:  # CRC32 (SSE4.2), destino de 32 ou 64 bits
            d = pick(R64) if MODE == 64 and bits == 64 else pick(R32)
            if bits == 8:
                return ["crc32b %s, %s" % (pick(R8), d)], 0, pre
            return ["crc32%s %s, %s" % (suf, pick(regs), d)], 0, pre
        if bits == 8:
            bits, suf, regs = 32, "l", R32
        return ["popcnt%s %s, %s" % (suf, pick(regs), pick(regs))], ALL, pre
    if kind == "string":
        op = rnd.choice(["movs", "stos", "lods", "scas", "cmps"])
        rep = rnd.choice(["", "rep ", "repne "]) if op in ("scas", "cmps") else rnd.choice(["", "rep "])
        s = rnd.choice(["b", "w", "l"] + (["q"] if MODE == 64 else []))
        si, di, cx = ("rsi", "rdi", "rcx") if MODE == 64 else ("esi", "edi", "ecx")
        pre = ["lea buf+%d, %%%s" % (rnd.randrange(0, 60), si), "lea buf+%d, %%%s" % (rnd.randrange(60, 120), di),
               "mov $%d, %%%s" % (rnd.randrange(0, 12), cx)]
        if rnd.random() < 0.3:
            pre.append("std")
            pre[0] = "lea buf+%d, %%%s" % (rnd.randrange(100, 160), si)
            pre[1] = "lea buf+%d, %%%s" % (rnd.randrange(160, 200), di)
        post = ["cld"] if "std" in pre else []
        m = ALL if op in ("scas", "cmps") else 0
        return ["%s%s%s" % (rep, op, s)] + post, m, pre
    if kind == "misc":
        c = rnd.choice(["xlat", "leave_like", "enter_like", "neg_adc"] + (["branch66"] * 2 if MODE == 64 else []))
        if c == "branch66":
            # desvios proximos com prefixo 66 (como o "66 66 48 e8" do TLS da glibc): no modo de
            # 64 bits a Intel ignora o 66; rcx = erro no endereco de retorno, rbx = erro na pilha
            call = rnd.choice([".byte 0x66, 0x66, 0x48, 0xe8; .long 0", ".byte 0x66, 0x48, 0xe8; .long 0"])
            br = rnd.choice(["", ".byte 0x66, 0xeb, 0x00", ".byte 0x66, 0xe9; .long 0", ".byte 0x66, 0x0f, 0x84; .long 0"])
            return ["mov %rsp, %rax", call, "91: pop %rcx", "lea 91b(%rip), %rbx", "sub %rbx, %rcx",
                    br, "lea 92f(%rip), %rbx", "push %rbx", ".byte 0x66, 0x48, 0xc3", "92: mov %rsp, %rbx",
                    "sub %rax, %rbx", "mov $0, %eax"], 0, []
        if c == "xlat":
            bx = "rbx" if MODE == 64 else "ebx"
            return ["xlat"], 0, ["lea buf, %%%s" % bx]
        if c == "neg_adc":
            r = pick(regs)
            return ["neg%s %s" % (suf, r), "adc%s %s, %s" % (suf, pick(regs), r)], ALL, pre
        return ["nop"], 0, pre
    if kind == "mmx":  # MMX (o cursor/GDI do XP): valores entram e saem pela memoria (buffer comparado)
        def m8():
            off = rnd.randrange(0, 24) * 8
            return "buf+%d" % off if MODE == 32 else "%d(%%r15)" % off
        g = rnd.choice(["paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq", "pand", "pandn", "por",
                        "pxor", "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd", "punpcklbw",
                        "punpcklwd", "punpckldq", "punpckhbw", "punpckhwd", "punpckhdq", "packsswb", "packuswb",
                        "packssdw", "pmullw", "pavgb", "pavgw", "pminub", "pmaxub", "pminsw", "pmaxsw", "paddsb",
                        "paddsw", "paddusb", "paddusw", "psubsb", "psubsw", "psubusb", "psubusw", "psllw", "pslld",
                        "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad", "movq", "movd"])
        ma, mb = "%%mm%d" % rnd.randrange(8), "%%mm%d" % rnd.randrange(8)
        lines = ["movq %s, %s" % (m8(), mb), "movq %s, %s" % (m8(), ma)]
        if g in ("psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"):
            lines.append("%s $%d, %s" % (g, rnd.randrange(0, 70), ma))
        elif g == "movd":
            r = "%" + rnd.choice(R32)
            lines += ["movd %s, %s" % (ma, r), "movd %s, %s" % (r, mb)] if rnd.random() < 0.5 else ["movd %s, %s" % (m8(), ma)]
        else:
            src = m8() if rnd.random() < 0.3 else mb
            lines.append("%s %s, %s" % (g, src, ma))
        return lines + ["movq %s, %s" % (ma, m8()), "movq %s, %s" % (mb, m8()), "emms"], 0, pre
    if kind == "sse":
        xs = ["%%xmm%d" % i for i in range(16 if MODE == 64 else 8)]
        op = rnd.choice(["paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq", "pand", "pandn",
                         "por", "pxor", "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpgtb", "pcmpgtw", "pcmpgtd",
                         "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq", "punpckhbw", "punpckhwd",
                         "punpckhdq", "punpckhqdq", "packsswb", "packuswb", "packssdw", "pmullw", "pmulhw",
                         "pmulhuw", "pmuludq", "pmaddwd", "psadbw", "pavgb", "pavgw", "pminub", "pmaxub",
                         "pminsw", "pmaxsw", "paddsb", "paddsw", "paddusb", "paddusw", "psubsb", "psubsw",
                         "psubusb", "psubusw", "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw",
                         "psrad", "movdqa", "movdqu", "movaps", "movups", "andps", "orps", "xorps", "andnps",
                         "addps", "subps", "mulps", "addpd", "subpd", "mulpd", "addss", "addsd", "mulsd",
                         "subsd", "minps", "maxps", "minsd", "maxsd", "unpcklps", "unpckhps", "unpcklpd",
                         "unpckhpd", "movss", "movsd", "cvtdq2ps", "cvttps2dq", "cvtps2pd", "cvtpd2ps",
                         "cvtss2sd", "cvtsd2ss", "sqrtpd", "sqrtss",
                         "pshufd", "pshuflw", "pshufhw", "shufps", "shufpd", "psrldq", "pslldq",
                         "pmovmskb", "movmskps", "movmskpd", "ucomisd", "comiss", "cvttsd2si", "cvtsi2sd",
                         "movd_r", "movq_x", "pinsrw", "pextrw", "cmpps", "cmpsd", "ssemem", "ssemem"] + ["sse4"] * 12)
        if os.environ.get("FUZZ_SSEOP"):
            op = os.environ["FUZZ_SSEOP"]
        a, b = rnd.choice(xs), rnd.choice(xs)
        if op in ("pshufd", "pshuflw", "pshufhw", "shufps", "shufpd", "cmpps", "cmpsd"):
            k = rnd.randrange(0, 8 if op.startswith("cmp") else 256)
            return ["%s $%d, %s, %s" % (op, k, a, b)], 0, pre
        if op in ("psrldq", "pslldq"):
            return ["%s $%d, %s" % (op, rnd.randrange(0, 17), a)], 0, pre
        if op in ("psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad") and rnd.random() < 0.5:
            return ["%s $%d, %s" % (op, rnd.randrange(0, 70), a)], 0, pre
        if op in ("pmovmskb", "movmskps", "movmskpd"):
            return ["%s %s, %s" % (op, a, "%" + rnd.choice(R32))], 0, pre
        if op in ("ucomisd", "comiss"):
            if rnd.random() < 0.5:  # SETcc logo depois: condicao avaliada pelo JIT a partir dos flags
                cc = rnd.choice(["b", "ae", "e", "ne", "be", "a", "p", "np", "s", "ns", "o", "no", "l", "ge", "le", "g"])
                return ["%s %s, %s" % (op, a, b), "set%s %%cl" % cc], ALL, pre
            return ["%s %s, %s" % (op, a, b)], ALL, pre
        if op == "cvttsd2si":
            return ["cvttsd2si %s, %s" % (a, "%" + rnd.choice(R32))], 0, pre
        if op == "cvtsi2sd":
            return ["cvtsi2sdl %s, %s" % ("%" + rnd.choice(R32), a)], 0, pre
        if op == "movd_r":
            return ["movd %s, %s" % ("%" + rnd.choice(R32), a) if rnd.random() < 0.5 else "movd %s, %s" % (a, "%" + rnd.choice(R32))], 0, pre
        if op == "movq_x":
            return ["movq %s, %s" % (a, b)], 0, pre
        if op == "sse4":  # SSE3 / SSSE3 / SSE4.1 / SSE4.2
            m16 = ("buf+%d" % (rnd.randrange(0, 12) * 16)) if MODE == 32 else ("%d(%%r15)" % (rnd.randrange(0, 12) * 16))
            src = m16 if rnd.random() < 0.25 else a
            gr = "%" + rnd.choice(R32)
            plain = ["addsubps", "addsubpd", "haddps", "haddpd", "hsubps", "hsubpd", "movsldup", "movshdup",
                     "movddup", "pshufb", "phaddw", "phaddd", "phaddsw", "phsubw", "phsubd", "phsubsw",
                     "pmaddubsw", "psignb", "psignw", "psignd", "pmulhrsw", "pabsb", "pabsw", "pabsd",
                     "pmovsxbw", "pmovsxbd", "pmovsxbq", "pmovsxwd", "pmovsxwq", "pmovsxdq", "pmovzxbw",
                     "pmovzxbd", "pmovzxbq", "pmovzxwd", "pmovzxwq", "pmovzxdq", "pmuldq", "pcmpeqq", "pcmpgtq",
                     "packusdw", "pminsb", "pminsd", "pminuw", "pminud", "pmaxsb", "pmaxsd", "pmaxuw", "pmaxud",
                     "pmulld", "phminposuw"]
            withimm = ["palignr", "roundps", "roundpd", "roundss", "roundsd", "blendps", "blendpd", "pblendw",
                       "dpps", "dppd", "mpsadbw", "insertps"]
            f = rnd.choice(plain * 2 + withimm * 2 + ["blendv", "ptest", "pextr", "pinsr", "pcmpstr", "pcmpstr",
                                                     "lddqu", "movntdqa", "mmx", "mmx"])
            if f in plain:
                return ["%s %s, %s" % (f, src, b)], 0, pre
            if f in withimm:
                k = rnd.randrange(0, 32) if f == "palignr" else rnd.randrange(0, 256)
                if f.startswith("round"):
                    k &= 0xf
                return ["%s $%d, %s, %s" % (f, k, src, b)], 0, pre
            if f == "blendv":
                return ["%s %%xmm0, %s, %s" % (rnd.choice(["pblendvb", "blendvps", "blendvpd"]), src, b)], 0, pre
            if f == "ptest":
                return ["ptest %s, %s" % (src, b)], ALL, pre
            if f == "pextr":
                g = rnd.choice(["pextrb", "pextrw", "pextrd", "extractps"] + (["pextrq"] if MODE == 64 else []))
                k = rnd.randrange(0, 16)
                dstop = ("%" + rnd.choice(["rax", "rbx", "rcx", "rdx", "rsi", "rdi"])) if g == "pextrq" else gr
                if rnd.random() < 0.3:
                    dstop = m16
                return ["%s $%d, %s, %s" % (g, k, a, dstop)], 0, pre
            if f == "pinsr":
                g = rnd.choice(["pinsrb", "pinsrd"] + (["pinsrq"] if MODE == 64 else []))
                k = rnd.randrange(0, 16)
                srcop = ("%" + rnd.choice(["rax", "rbx", "rcx", "rdx", "rsi", "rdi"])) if g == "pinsrq" else gr
                if rnd.random() < 0.3:
                    srcop = m16
                return ["%s $%d, %s, %s" % (g, k, srcop, a)], 0, pre
            if f == "pcmpstr":
                g = rnd.choice(["pcmpestri", "pcmpestrm", "pcmpistri", "pcmpistrm"])
                k = rnd.randrange(0, 128)
                lines = []
                if g.startswith("pcmpe"):
                    ax, dx = ("eax", "edx")
                    lines = ["mov $%d, %%%s" % (rnd.randrange(-20, 20), ax), "mov $%d, %%%s" % (rnd.randrange(-20, 20), dx)]
                return lines + ["%s $%d, %s, %s" % (g, k, src, a)], ALL, pre
            if f in ("lddqu", "movntdqa"):
                return ["%s %s, %s" % (f, m16, b)], 0, pre
            if f == "mmx":  # formas MMX do SSSE3: valores via memoria (o buffer e comparado)
                def m8():
                    off = rnd.randrange(0, 24) * 8
                    return "buf+%d" % off if MODE == 32 else "%d(%%r15)" % off
                g = rnd.choice(["pshufb", "phaddw", "phaddd", "phaddsw", "phsubw", "phsubd", "phsubsw", "pmaddubsw",
                                "psignb", "psignw", "psignd", "pmulhrsw", "pabsb", "pabsw", "pabsd", "palignr"])
                ma, mb = "%%mm%d" % rnd.randrange(8), "%%mm%d" % rnd.randrange(8)
                opline = ("palignr $%d, %s, %s" % (rnd.randrange(0, 20), mb, ma)) if g == "palignr" else ("%s %s, %s" % (g, mb, ma))
                return ["movq %s, %s" % (m8(), mb), "movq %s, %s" % (m8(), ma), opline, "movq %s, %s" % (ma, m8()), "emms"], 0, pre
        if op == "ssemem":  # movimentos SSE de/para memoria
            def m(al):
                off = rnd.randrange(0, 12) * 16 + (0 if al else rnd.randrange(0, 16))
                return "buf+%d" % off if MODE == 32 else "%d(%%r15)" % off
            f = rnd.choice(["movss", "movsd", "movq", "movd", "movups", "movdqu", "movaps", "movdqa", "movntps",
                            "movntdq", "movlps", "movhps"] + (["movq_r64"] if MODE == 64 else []))
            if f == "movq_r64":
                r = "%" + rnd.choice(["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"])
                return ["movq %s, %s" % (r, a) if rnd.random() < 0.5 else "movq %s, %s" % (a, r)], 0, pre
            al = f in ("movaps", "movdqa", "movntps", "movntdq")
            if f.startswith("movnt") or rnd.random() < 0.5:
                return ["%s %s, %s" % (f, a, m(al))], 0, pre
            return ["%s %s, %s" % (f, m(al), a)], 0, pre
        if op == "pinsrw":
            return ["pinsrw $%d, %s, %s" % (rnd.randrange(0, 8), "%" + rnd.choice(R32), a)], 0, pre
        if op == "pextrw":
            return ["pextrw $%d, %s, %s" % (rnd.randrange(0, 8), a, "%" + rnd.choice(R32))], 0, pre
        if rnd.random() < 0.3 and op not in ("movss", "movsd"):
            src = "buf+%d" % (rnd.randrange(0, 12) * 16) if MODE == 32 else "%d(%%r15)" % (rnd.randrange(0, 12) * 16)
            return ["%s %s, %s" % (op, src, b)], 0, pre
        return ["%s %s, %s" % (op, a, b)], 0, pre
    return ["nop"], 0, pre


def regs_init():
    lines = []
    for r in FULL:
        v = val(64 if MODE == 64 else 32)
        lines.append("mov $%d, %%%s" % (v, r) if MODE == 64 else "mov $%d, %%%s" % (v, r))
    if MODE == 64:
        lines.append("mov $buf, %r15")
    return lines


with open(OUT + "/fuzz.S", "w") as f:
    f.write(".text\n.globl run_tests\nrun_tests:\n")
    if MODE == 64:
        f.write("push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\n")
    else:
        f.write("push %ebx\npush %ebp\npush %esi\npush %edi\n")
    for t in range(N):
        body, mask, pre = gen()
        f.write("# teste %d\n" % t)
        for x in regs_init():
            f.write(x + "\n")
        # xmm iniciais a partir do buffer
        nx = 16 if MODE == 64 else 8
        for i in range(nx):
            f.write("movdqu buf+%d, %%xmm%d\n" % (rnd.randrange(0, 13) * 16, i))
        flags = rnd.getrandbits(12) & 0x8d5
        if MODE == 64:
            f.write("pushq $%d\npopfq\n" % flags)
        else:
            f.write("pushl $%d\npopfl\n" % flags)
        for x in pre:
            f.write(x + "\n")
        for x in body:
            f.write(x + "\n")
        # salva estado (sem alterar flags)
        if MODE == 64:
            f.write("pushfq\npopq sv_flags\n")
            for i, r in enumerate(FULL):
                f.write("mov %%%s, sv_regs+%d\n" % (r, 8 * i))
            for i in range(16):
                f.write("movdqu %%xmm%d, sv_xmm+%d\n" % (i, 16 * i))
            f.write("mov $%d, %%edi\nmov $%d, %%esi\ncall dump\n" % (t, mask))
        else:
            f.write("pushfl\npopl sv_flags\n")
            for i, r in enumerate(FULL):
                f.write("mov %%%s, sv_regs+%d\n" % (r, 8 * i))
            for i in range(8):
                f.write("movdqu %%xmm%d, sv_xmm+%d\n" % (i, 16 * i))
            f.write("pushl $%d\npushl $%d\ncall dump\naddl $8, %%esp\n" % (mask, t))
        f.write("cld\n")
    if MODE == 64:
        f.write("pop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n")
    else:
        f.write("pop %edi\npop %esi\npop %ebp\npop %ebx\nret\n")
    # buffer inicial aleatorio
    f.write(".data\n.balign 64\n.globl buf\nbuf:\n")
    for i in range(0, 256):
        f.write(".byte %d\n" % rnd.getrandbits(8))
    f.write(".space 256\n")
