#!/bin/sh
# Midias com a VM ligada na placa ARM64 "virt" com UEFI (virtio-scsi), Alpine "virt" aarch64:
#  1) Boot pela ISO em virtio-blk: o UEFI cria um CD SCSI vazio. Insere um "CD" de dados
#     aleatorios, confere o md5 e ejeta; conecta um disco com a VM ligada (hot-plug), confere
#     o md5 e desconecta.
#  2) Boot pela ISO no CD SCSI (edk2 VirtioScsiDxe + Linux virtio_scsi/sr_mod).
# Uso: ./run_media_arm_tests.sh   (~6 min)
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
FW=${FW:-../../android/app/src/main/assets/bios}
ISO=${ISO:-/root/multivm-images/uefi-arm/alpine-virt-3.24.2-aarch64.iso}
mkdir -p out
python3 -c "
import os
open('out/acd.bin','wb').write(os.urandom(2048*1024))     # 2 MiB
open('out/ahd.img','wb').write(os.urandom(8 << 20))       # 8 MiB
"
md5() { md5sum < "$1" | cut -d' ' -f1; }
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FALHA $1"; fail=1; fi; }
wait_for() { t=0; until grep -a -q "$2" "$1" 2>/dev/null || [ $t -ge "$3" ]; do sleep 2; t=$((t + 2)); done; }

run_vm() { # nome, opcoes...; comandos em out/$1.cmds ("S" serial, "C" controle, "W" espera)
    name=$1; shift
    log=out/$name.log
    rm -f $log out/$name.ctl
    mkfifo out/$name.ctl
    (
        exec 3>out/$name.ctl
        wait_for $log "login:" 400; sleep 2; printf 'root\n'; sleep 6
        while IFS= read -r line; do
            kind=${line%% *}; arg=${line#* }
            case $kind in
                S) printf '%s\n' "$arg"; sleep 1 ;;
                C) echo "$arg" >&3; sleep 2 ;;
                W) wait_for $log "$arg" 120 ;;
            esac
        done < out/$name.cmds
        sleep 5
    ) | "$MVM" -a arm64 -m 1024 --bios "$FW/AAVMF_CODE.fd" --net none --control out/$name.ctl "$@" -t 700 \
        -e MEDIA-TEST-DONE > $log 2>&1
    rm -f out/$name.ctl
}

cat > out/arm1.cmds <<'CMDS'
S modprobe virtio_scsi; modprobe sr_mod; modprobe sd_mod; sleep 3; ls /dev/sr0 && dd if=/dev/sr0 of=/dev/null bs=2048 count=1 2>&1 | grep -q -i medium && echo CD-EMPTY-""OK
W CD-EMPTY-OK
C media list
C media insert 1 out/acd.bin
S sleep 3; dd if=/dev/sr0 bs=2048 2>/dev/null | md5sum | sed "s/^/CD""MD5 /"
W CDMD5
C media eject 1
S sleep 3; dd if=/dev/sr0 of=/dev/null bs=2048 count=1 2>&1 | grep -q -i medium && echo CD-EJECT-""OK
W CD-EJECT-OK
C media add out/ahd.img
S sleep 4; dmesg | tail -8; ls /dev/sd*; dd if=/dev/sda bs=1M 2>/dev/null | md5sum | sed "s/^/HD""MD5 /"
W HDMD5
C media list
C media eject 2
S sleep 4; ls /dev/sda 2>/dev/null || echo HD-GONE-""OK
W HD-GONE-OK
S echo MEDIA-TEST-""DONE
CMDS
run_vm arm1 -r "$ISO"
L=$(tr -d '\r' < out/arm1.log)
check "arm64: CD SCSI vazio criado pelo UEFI" "echo \"\$L\" | grep -q '^CD-EMPTY-OK'"
check "arm64: CD inserido com a VM ligada (md5)" "echo \"\$L\" | grep -q \"^CDMD5 $(md5 out/acd.bin)\""
check "arm64: CD ejetado" "echo \"\$L\" | grep -q '^CD-EJECT-OK'"
check "arm64: disco conectado com a VM ligada (md5)" "echo \"\$L\" | grep -q \"^HDMD5 $(md5 out/ahd.img)\""
check "arm64: disco desconectado" "echo \"\$L\" | grep -q '^HD-GONE-OK'"

cat > out/arm2.cmds <<'CMDS'
S grep -q sr0 /proc/mounts && echo BOOT-SCSI-""OK; echo MEDIA-TEST-""DONE
CMDS
[ -n "$ONLY1" ] || run_vm arm2 --cdrom "$ISO"
check "arm64: boot pela ISO no CD SCSI" "tr -d '\r' < out/arm2.log | grep -q '^BOOT-SCSI-OK'"

[ $fail = 0 ] && echo "midias arm64: tudo ok" || echo "midias arm64: houve falhas (logs em tests/media/out)"
exit $fail
