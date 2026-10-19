#!/bin/sh
# UEFI no ARM64 e no ARM 32 bits (edk2 ArmVirtQemu) com as ISOs "virt" do Alpine (GRUB EFI):
#  1) firmware + copia das variaveis: Linux em modo EFI (fw_platform_size 64 ou 32), grava uma
#     variavel pelo efivarfs e desliga por PSCI (SYSTEM_OFF).
#  2) Mesmo arquivo de variaveis: a variavel tem de estar la; reinicia (PSCI SYSTEM_RESET) com
#     --no-reboot (codigo de saida 3).
# O kernel armv7 "virt" do Alpine e LPAE: o teste 1 do ARM tambem cobre a MMU com descritores longos.
# Uso: ./run_uefi_arm_tests.sh [arm64|arm]   (~12 min os dois; FW=dir dos .fd, ISODIR=dir das ISOs)
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
FW=${FW:-../../android/app/src/main/assets/bios}
ISODIR=${ISODIR:-/root/multivm-images/uefi-arm}
GUID=3e1f5c2a-7d0b-4b8e-9c1d-4d554c544956
mkdir -p out
fail=0

# nome, arch, iso, comandos (uma linha), opcoes do mvm-cli...: entra como root pela serial
run_vm() {
    name=$1 arch=$2 iso=$3 cmds=$4; shift 4
    log=out/$name.log
    rm -f $log
    (
        t=0; until grep -a -q "login:" $log 2>/dev/null || [ $t -ge 400 ]; do sleep 2; t=$((t + 2)); done
        sleep 2; printf 'root\n'; sleep 5
        printf '%s\n' "$cmds"; sleep 60
    ) | "$MVM" -a $arch -m 1024 -r "$iso" --net none -t 520 "$@" > $log 2>&1
    echo $? > out/$name.rc
}
clean() { tr -d '\r' < out/$1.log; }
check() { if eval "$2"; then echo "ok   $1"; else echo "FALHA $1"; fail=1; fi; }

EFI='grep -q efivarfs /proc/mounts || mount -t efivarfs none /sys/firmware/efi/efivars; V=/sys/firmware/efi/efivars'
# o "" no meio dos marcadores impede que o eco do comando digitado case com a espera

for arch in ${1:-arm64 arm}; do
    if [ $arch = arm64 ]; then
        code=$FW/AAVMF_CODE.fd vars=$FW/AAVMF_VARS.fd iso=$ISODIR/alpine-virt-3.24.2-aarch64.iso bits=64
    else
        code=$FW/AAVMF32_CODE.fd vars=$FW/AAVMF32_VARS.fd iso=$ISODIR/alpine-virt-3.24.2-armv7.iso bits=32
    fi
    cp "$vars" out/vars-$arch.fd
    run_vm grava-$arch $arch "$iso" "$EFI; echo PLAT-\$(cat /sys/firmware/efi/fw_platform_size); printf '\\007\\000\\000\\000MVM-PERSIST' > \$V/MultiVMTeste-$GUID && echo GRAVOU-\"\"OK; sync; poweroff" \
        --bios "$code" --uefi-vars out/vars-$arch.fd
    check "$arch: Linux em modo EFI de $bits bits" "clean grava-$arch | grep -q '^PLAT-$bits'"
    check "$arch: variavel gravada pelo efivarfs" "clean grava-$arch | grep -q '^GRAVOU-OK'"
    check "$arch: desligamento por PSCI" "grep -q 'PSCI: SYSTEM_OFF' out/grava-$arch.log"
    check "$arch: arquivo de variaveis alterado" "! cmp -s '$vars' out/vars-$arch.fd"

    run_vm le-$arch $arch "$iso" "$EFI; tail -c +5 \$V/MultiVMTeste-$GUID | sed 's/^/LIDO-/'; echo; reboot" \
        --bios "$code" --uefi-vars out/vars-$arch.fd --no-reboot
    check "$arch: variavel persistiu entre boots" "clean le-$arch | grep -q '^LIDO-MVM-PERSIST'"
    check "$arch: reinicio pelo convidado (--no-reboot sai com 3)" "[ \"\$(cat out/le-$arch.rc)\" = 3 ]"
done

[ $fail = 0 ] && echo "uefi arm: tudo ok" || echo "uefi arm: houve falhas (logs em tests/uefi/out)"
exit $fail
