#!/bin/sh
# Executa o fuzzer diferencial x86: run_x86.sh <64|32> <semente> <n>
set -e
cd "$(dirname "$0")"
MODE=$1; SEED=$2; N=${3:-2000}
CLANG=${CLANG:-clang}; LLD=${LLD:-ld.lld}
MVM=${MVM:-../../core/build/mvm-cli}
W=out/$MODE-$SEED
mkdir -p $W
python3 x86_fuzz.py $MODE $SEED $N $W
if [ "$MODE" = 64 ]; then T=x86_64-none-elf; M=elf_x86_64; Q=qemu-x86_64; A=x86_64; else T=i386-none-elf; M=elf_i386; Q=qemu-i386; A=i386; fi
F="-O2 -ffreestanding -nostdlib -fno-pic -fno-stack-protector -mno-sse -mno-red-zone"
$CLANG --target=$T $F $DUMPFLAGS -c dump.c -o $W/dump.o
$CLANG --target=$T -c $W/fuzz.S -o $W/fuzz.o
$CLANG --target=$T -DLINUX -c plat.S -o $W/plat_linux.o
$CLANG --target=$T -c plat.S -o $W/plat_bare.o
$LLD -m $M -static -Ttext=0x400000 --section-start=.data=0x800000 -e _start -o $W/linux.elf $W/plat_linux.o $W/fuzz.o $W/dump.o
$LLD -m $M -T ../guest/common/link.ld --defsym=LOAD_ADDR=0x300000 --section-start=.data=0x800000 -o $W/bare.elf $W/plat_bare.o $W/fuzz.o $W/dump.o
$Q $W/linux.elf > $W/ref.txt
timeout 120 $MVM -a $A -k $W/bare.elf -t 100 < /dev/null 2>/dev/null | tr -d '\r' | grep -E '^[0-9a-f]{5} |FUZZ END' > $W/mvm.txt || true
if diff -q $W/ref.txt $W/mvm.txt > /dev/null; then
    echo "OK: $N testes identicos ($MODE bits, semente $SEED)"
else
    echo "DIFERENCAS ($MODE bits, semente $SEED):"
    diff $W/ref.txt $W/mvm.txt | head -20
    exit 1
fi
