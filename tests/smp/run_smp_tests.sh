#!/bin/sh
# Teste de atomicidade/ordem do modo SMP: uma thread do host (MVM_STRESS_ATOMIC) faz o
# papel de outra CPU disputando contadores, um spinlock e uma passagem de mensagem com o
# convidado. Requer clang (alvo x86_64) e ld.lld.
set -e
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
OUT=out
mkdir -p $OUT
F=../fuzz
T=x86_64-none-elf
GN=2000000
clang --target=$T -O2 -ffreestanding -nostdlib -fno-pic -fno-stack-protector -mno-red-zone -mno-sse -DGN=$GN -c atomtest.c -o $OUT/at.o
clang --target=$T -c $F/plat.S -o $OUT/pb.o
ld.lld -m elf_x86_64 -T ../guest/common/link.ld --defsym=LOAD_ADDR=0x300000 -o $OUT/at.elf $OUT/pb.o $OUT/at.o
fail=0
for jit in 1 0; do
    for n in 500000 3000000; do
        exp=$(printf '%08x' $((GN + n)))
        r=$(MVM_JIT=$jit MVM_SMP_FORCE=1 MVM_STRESS_ATOMIC=0x700000:$n timeout 300 $MVM -a x86_64 -k $OUT/at.elf -t 250 </dev/null 2>&1 | tr -d '\r')
        atom=$(echo "$r" | sed -n 's/^ATOM //p'); lock=$(echo "$r" | sed -n 's/^LOCK //p'); viol=$(echo "$r" | sed -n 's/^VIOL //p')
        c64=$(echo "$r" | sed -n 's/^C64 //p'); c32=$(echo "$r" | sed -n 's/^C32 //p'); c16=$(echo "$r" | sed -n 's/^C16 //p')
        e3=$(printf '%08x' $((GN * 4 + n))); e16=$(printf '%08x' $(((GN * 4 + n) & 65535)))
        if [ "$atom" = "$exp" ] && [ "$lock" = "$exp" ] && [ "$viol" = "00000000" ] && [ "$c64" = "$e3" ] &&
           [ "$c32" = "$e3" ] && [ "$c16" = "$e16" ]; then
            echo "ok   jit=$jit host=$n (atomico $atom, spinlock $lock, ordem ok, xadd/inc/add/sub/dec/cmpxchg $c64)"
        else
            echo "FALHA jit=$jit host=$n: esperado $exp/$e3, atomico '$atom' spinlock '$lock' violacoes '$viol' c64 '$c64' c32 '$c32' c16 '$c16'"; fail=1
        fi
    done
done
exit $fail
