#!/bin/sh
# Linux com o driver floppy: boota a ISO do Alpine (modloop tem o floppy.ko), entra
# como root pela serial, le /dev/fd0 (md5 tem de bater com a imagem) e grava um setor.
# Uso: ISO=alpine-virt-x86_64.iso ./run_linux_floppy.sh   (leva ~2 min)
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
BIOS=${BIOS:-/usr/share/seabios/bios-256k.bin}
ISO=${ISO:-/sdcard/Download/MultiVM/alpine-virt-3.24.2-x86_64.iso}
BOOT_WAIT=${BOOT_WAIT:-100}
mkdir -p out
python3 -c "import os; open('out/linux.img','wb').write(os.urandom(1474560))"
want=$(md5sum < out/linux.img | cut -d' ' -f1)
( sleep "$BOOT_WAIT"; printf 'root\n'; sleep 8
  printf 'modprobe floppy; sleep 2; dd if=/dev/fd0 bs=512 count=2880 2>/dev/null | md5sum | sed "s/^/MD5 /"; '
  printf 'echo MULTIVM-FD-OK | dd of=/dev/fd0 bs=512 seek=100 conv=sync 2>/dev/null; sync; echo FD-TEST-""DONE\n'
  sleep 300 ) | "$MVM" -a x86_64 -m 512 --bios "$BIOS" --cdrom "$ISO" --fda out/linux.img -t 360 -e FD-TEST-DONE \
    > out/linux.log 2>&1
got=$(tr -d '\r' < out/linux.log | grep '^MD5 ' | awk '{print $2}')
wrote=$(dd if=out/linux.img bs=512 skip=100 count=1 2>/dev/null | head -c 13)
ok=ok
[ "$got" = "$want" ] || ok=FALHOU
[ "$wrote" = MULTIVM-FD-OK ] || ok=FALHOU
echo "linux-floppy $ok  leitura:${got:-?}  escrita:${wrote:-?}"
[ $ok = ok ]
