#!/bin/sh
# Testes do JIT x86: instrucoes que cruzam o fim de pagina (CALL e ADD imm32 nativos com
# conferencia dos bytes da pagina seguinte, inclusive depois de reescritos; BSWAP com o
# opcode dividido entre as paginas). Requer clang e ld.lld.
set -e
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
OUT=out
mkdir -p $OUT
T=x86_64-none-elf
clang --target=$T -O2 -ffreestanding -nostdlib -fno-pic -fno-stack-protector -mno-red-zone -mno-sse -c xpage.c -o $OUT/xp.o
clang --target=$T -c ../fuzz/plat.S -o $OUT/pb.o
ld.lld -m elf_x86_64 -T ../guest/common/link.ld --defsym=LOAD_ADDR=0x300000 -o $OUT/xp.elf $OUT/pb.o $OUT/xp.o
want="XP1 a00186a0 XP2 d00003e8 XP3 00000000"
fail=0
for off in 0 8192; do
    for jit in 1 0; do
        got=$(MVM_JIT=$jit MVM_JIT_OFF=$off timeout 60 $MVM -a x86_64 -k $OUT/xp.elf -t 30 </dev/null 2>/dev/null | tr -d '\r' | grep '^XP' | tr '\n' ' ' | sed 's/ $//')
        if [ "$got" = "$want" ]; then echo "ok   cruza pagina (jit=$jit off=$off)"; else echo "FALHA cruza pagina (jit=$jit off=$off): '$got'"; fail=1; fi
    done
done
exit $fail
