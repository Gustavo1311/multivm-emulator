#!/bin/sh
# UEFI (OVMF) com o live CD do SliTaz, que so tem boot EFI (El Torito com plataforma 0xEF):
#  1) OVMF.fd unico (variaveis so na RAM): GRUB EFI -> Linux, confere /sys/firmware/efi.
#  2) OVMF_CODE + copia do OVMF_VARS (--uefi-vars): grava uma variavel pelo efivarfs e desliga
#     por ACPI (S5 no bloco PM em 0xB000).
#  3) Mesmo arquivo de variaveis: a variavel tem de estar la; reinicia (reset do OVMF) com
#     --no-reboot (codigo de saida 3).
# Uso: ./run_uefi_tests.sh   (leva ~5 min; ISO=..., CODE=..., VARS=..., OVMF=... para trocar)
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
FW=../../android/app/src/main/assets/bios
CODE=${CODE:-$FW/OVMF_CODE.fd}
VARS=${VARS:-$FW/OVMF_VARS.fd}
OVMF=${OVMF:-/usr/share/ovmf/OVMF.fd}
ISO=${ISO:-/sdcard/emulator/iso/slitaz-base-x86_64-cooking.iso}
GUID=3e1f5c2a-7d0b-4b8e-9c1d-4d554c544956
mkdir -p out
fail=0

# nome, comandos (uma linha), opcoes do mvm-cli...: entra como root pela serial e roda os comandos
run_vm() {
    name=$1 cmds=$2; shift 2
    log=out/$name.log
    rm -f $log
    (
        t=0; until grep -a -q "login:" $log 2>/dev/null || [ $t -ge 300 ]; do sleep 2; t=$((t + 2)); done
        sleep 2; printf 'root\n'; sleep 3; printf 'root\n'; sleep 5
        printf '%s\n' "$cmds"; sleep 60
    ) | "$MVM" -a x86_64 -m 1024 --cdrom "$ISO" -t 420 "$@" > $log 2>&1
    echo $? > out/$name.rc
}
clean() { tr -d '\r' < out/$1.log; }
check() { if eval "$2"; then echo "ok   $1"; else echo "FALHA $1"; fail=1; fi; }

EFI='grep -q efivarfs /proc/mounts || mount -t efivarfs none /sys/firmware/efi/efivars; V=/sys/firmware/efi/efivars'
# o "" no meio dos marcadores impede que o eco do comando digitado case com a espera

run_vm combinado "$EFI; echo PLAT-\$(cat /sys/firmware/efi/fw_platform_size); ls \$V | grep -c '^Boot' | sed 's/^/BOOTVARS-/'; echo UEFI-\"\"FIM" \
    --bios "$OVMF" -e UEFI-FIM
check "OVMF.fd: Linux em modo EFI de 64 bits" "clean combinado | grep -q '^PLAT-64'"
check "OVMF.fd: variaveis de boot visiveis" "clean combinado | grep -q '^BOOTVARS-[1-9]'"

cp "$VARS" out/vars.fd
run_vm grava "$EFI; printf '\\007\\000\\000\\000MVM-PERSIST' > \$V/MultiVMTeste-$GUID && echo GRAVOU-\"\"OK; sync; poweroff" \
    --bios "$CODE" --uefi-vars out/vars.fd
check "variavel gravada pelo efivarfs" "clean grava | grep -q '^GRAVOU-OK'"
check "desligamento ACPI (S5)" "grep -q 'ACPI: desligamento (S5)' out/grava.log"
check "arquivo de variaveis alterado" "! cmp -s '$VARS' out/vars.fd"

run_vm le "$EFI; tail -c +5 \$V/MultiVMTeste-$GUID | sed 's/^/LIDO-/'; echo; reboot" \
    --bios "$CODE" --uefi-vars out/vars.fd --no-reboot
check "variavel persistiu entre boots" "clean le | grep -q '^LIDO-MVM-PERSIST'"
check "reinicio pelo convidado (--no-reboot sai com 3)" "[ \"\$(cat out/le.rc)\" = 3 ]"

[ $fail = 0 ] && echo "uefi: tudo ok" || echo "uefi: houve falhas (logs em tests/uefi/out)"
exit $fail
