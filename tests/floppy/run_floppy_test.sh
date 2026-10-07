#!/bin/sh
# Teste do controlador de disquete: boot pelo A: com SeaBIOS, leitura de varias
# trilhas/faces e gravacao via INT 13h. Requer clang (alvo i386) e ld.lld.
set -e
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
BIOS=${BIOS:-/usr/share/seabios/bios-256k.bin}
OUT=out
mkdir -p $OUT
clang --target=i386-none-elf -c boot.S -o $OUT/boot.o
ld.lld -m elf_i386 --image-base=0 -Ttext=0x7c00 --oformat=binary -e _start $OUT/boot.o -o $OUT/boot.bin
python3 mkimg.py $OUT/boot.bin $OUT/floppy.img

fail=0
run() { # nome, opcoes extras...
    name=$1; shift
    cp $OUT/floppy.img $OUT/test.img
    if $MVM -a i386 -m 32 --bios "$BIOS" "$@" -t 30 -e "FLOPPY OK" > $OUT/$name.log 2>&1; then
        echo "ok   $name"
    else
        echo "FALHA $name (veja $OUT/$name.log)"; fail=1
    fi
}

run boot-a --fda $OUT/test.img --boot a
# o setor gravado tem de estar na imagem
python3 -c "
d=open('$OUT/test.img','rb').read()
assert d[2879*512:2880*512]==b'\xa5'*512, 'gravacao nao chegou a imagem'
" && echo "ok   gravacao persistida" || { echo "FALHA gravacao persistida"; fail=1; }

# disquete protegido: a leitura funciona, a gravacao tem de falhar (FAIL 5)
cp $OUT/floppy.img $OUT/test.img
$MVM -a i386 -m 32 --bios "$BIOS" --fda-ro $OUT/test.img --boot a -t 30 -e "FLOPPY FAIL 5" > $OUT/ro.log 2>&1 \
    && echo "ok   protecao contra escrita" || { echo "FALHA protecao contra escrita (veja $OUT/ro.log)"; fail=1; }

# boot automatico (sem CD e com disco vazio, o disquete e o ultimo recurso)
run boot-auto --fda $OUT/test.img
exit $fail
