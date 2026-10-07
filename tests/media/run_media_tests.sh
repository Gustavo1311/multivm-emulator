#!/bin/sh
# Troca de midias com a VM ligada (Alpine standard, comandos pela serial e pelo
# FIFO de controle):
#  1) IDE: CD extra vazio e drive A: vazio; insere um "CD" e um disquete de dados
#     aleatorios, confere o md5 lido pelo convidado e ejeta o CD.
#  2) SATA: conecta um disco com a VM ligada (hot-plug), confere o md5 e desconecta.
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
BIOS=${BIOS:-/usr/share/seabios/bios-256k.bin}
ISO=${ISO:-/root/multivm-images/alpine/alpine-standard-3.24.2-x86_64.iso}
BOOT_WAIT=${BOOT_WAIT:-150}
mkdir -p out
python3 -c "
import os
open('out/cd.bin','wb').write(os.urandom(2048*1024))     # 2 MiB
open('out/fd.img','wb').write(os.urandom(1474560))       # 1,44 MiB
open('out/hd.img','wb').write(os.urandom(8 << 20))       # 8 MiB
"
md5() { md5sum < "$1" | cut -d' ' -f1; }
fail=0

# espera TEXTO no log (ate N s)
wait_for() { t=0; until grep -a -q "$2" "$1" 2>/dev/null || [ $t -ge "$3" ]; do sleep 2; t=$((t + 2)); done; }

run_vm() { # nome, opcoes...; le os comandos de out/$1.cmds (linhas "S texto" = serial, "C cmd" = controle, "W texto" = espera)
    name=$1; shift
    log=out/$name.log
    rm -f $log out/$name.ctl
    mkfifo out/$name.ctl
    (
        exec 3>out/$name.ctl
        wait_for $log "login:" $((BOOT_WAIT * 3)); sleep 2; printf 'root\n'; sleep 6
        while IFS= read -r line; do
            kind=${line%% *}; arg=${line#* }
            case $kind in
                S) printf '%s\n' "$arg"; sleep 1 ;;
                C) echo "$arg" >&3; sleep 2 ;;
                W) wait_for $log "$arg" 120 ;;
            esac
        done < out/$name.cmds
        sleep 5
    ) | "$MVM" -a x86_64 -m 768 --bios "$BIOS" --control out/$name.ctl "$@" -t 900 -e MEDIA-TEST-DONE > $log 2>&1
    rm -f out/$name.ctl
}

if [ -z "$ONLY_SATA" ]; then
cat > out/ide.cmds <<'CMDS'
S modprobe sr_mod; modprobe floppy; sleep 2; dd if=/dev/sr1 of=/dev/null bs=2048 count=1 2>&1 | grep -q -i medium && echo CD-EMPTY-""OK
W CD-EMPTY-OK
C media list
C media insert 1 out/cd.bin
S sleep 3; dd if=/dev/sr1 bs=2048 2>/dev/null | md5sum | sed "s/^/CD""MD5 /"
W CDMD5
C media insert 2 out/fd.img
S sleep 2; dd if=/dev/fd0 bs=512 count=2880 2>/dev/null | md5sum | sed "s/^/FD""MD5 /"
W FDMD5
C media eject 1
S sleep 3; dd if=/dev/sr1 of=/dev/null bs=2048 count=1 2>&1 | grep -q -i medium && echo CD-EJECT-""OK
W CD-EJECT-OK
S echo MEDIA-TEST-""DONE
CMDS
run_vm ide --cdrom "$ISO" --cdrom none --fda none
clean=$(tr -d '\r' < out/ide.log)
echo "$clean" | grep -q "^CD-EMPTY-OK" && echo "ok   CD vazio sem midia" || { echo "FALHA CD vazio"; fail=1; }
[ "$(echo "$clean" | grep '^CDMD5' | awk '{print $2}')" = "$(md5 out/cd.bin)" ] && echo "ok   CD inserido (md5)" || { echo "FALHA CD inserido"; fail=1; }
[ "$(echo "$clean" | grep '^FDMD5' | awk '{print $2}')" = "$(md5 out/fd.img)" ] && echo "ok   disquete inserido (md5)" || { echo "FALHA disquete inserido"; fail=1; }
echo "$clean" | grep -q "^CD-EJECT-OK" && echo "ok   CD ejetado" || { echo "FALHA CD ejetado"; fail=1; }
fi

cat > out/sata.cmds <<'CMDS'
S ls /sys/block | tr "\n" " " | sed "s/^/AN""TES /"; echo
W ANTES
C media add out/hd.img
S sleep 8; ls /sys/block | tr "\n" " " | sed "s/^/DE""POIS /"; echo; dd if=/dev/sda bs=1M 2>/dev/null | md5sum | sed "s/^/HD""MD5 /"
W HDMD5
C media list
C media eject 1
S sleep 40; ls /sys/block | tr "\n" " " | sed "s/^/RE""MOVIDO /"; echo; dmesg | tail -25
W REMOVIDO
S echo MEDIA-TEST-""DONE
CMDS
run_vm sata --cdrom "$ISO" --sata
clean=$(tr -d '\r' < out/sata.log)
echo "$clean" | grep "^DEPOIS" | grep -q " sda" && echo "ok   disco SATA conectado (hot-plug)" || { echo "FALHA hot-plug SATA"; fail=1; }
[ "$(echo "$clean" | grep '^HDMD5' | awk '{print $2}')" = "$(md5 out/hd.img)" ] && echo "ok   disco SATA lido (md5)" || { echo "FALHA leitura do disco SATA"; fail=1; }
echo "$clean" | grep "^REMOVIDO" | grep -q " sda" && { echo "FALHA disco SATA continua presente"; fail=1; } || echo "ok   disco SATA desconectado"
exit $fail
