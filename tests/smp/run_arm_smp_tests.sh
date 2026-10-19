#!/bin/sh
# Varias CPUs no ARM64 e no ARMv7 (placa virt + UEFI, Alpine "virt" aarch64/armv7, comandos pela serial):
#  - o kernel liga as CPUs extras pelo PSCI CPU_ON (ACPI/MADT) e ve N processadores;
#  - 4 tarefas iguais em paralelo dao o mesmo md5 (atomicos, TLBI entre CPUs, IPIs);
#  - 300 processos criados em paralelo e nenhum Oops/BUG/travamento no dmesg.
# Mostra tambem o tempo de 1 tarefa e de 4 em paralelo.
# Uso: ./run_arm_smp_tests.sh [N] [arm64|arm]  (~5 min no arm64; o arm, so interpretador, demora mais)
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
FW=${FW:-../../android/app/src/main/assets/bios}
N=${1:-4}
ARCH=${2:-${ARCH:-arm64}}
if [ "$ARCH" = arm ]; then
    ISO=${ISO:-/root/multivm-images/uefi-arm/alpine-virt-3.24.2-armv7.iso}
    BIOS=$FW/AAVMF32_CODE.fd
else
    ISO=${ISO:-/root/multivm-images/uefi-arm/alpine-virt-3.24.2-aarch64.iso}
    BIOS=$FW/AAVMF_CODE.fd
fi
TMO=${TMO:-600}
mkdir -p out
log=out/$ARCH-smp.log
rm -f $log
CMDS='dmesg | grep -o "SMP: Total of [0-9]* processors activated"; nproc | sed "s/^/NPROC-/"; J="dd if=/dev/zero bs=1M count=200 2>/dev/null | md5sum"; t0=$(date +%s); sh -c "$J" > /tmp/r0; t1=$(date +%s); echo UMA-$((t1-t0))s; for i in 1 2 3 4; do sh -c "$J" > /tmp/r$i & done; wait; t2=$(date +%s); echo QUATRO-$((t2-t1))s; cat /tmp/r* | sort -u | wc -l | sed "s/^/MD5S-/"; for i in $(seq 300); do /bin/true & done; wait; echo FORK-""OK; dmesg | grep -c -E "Oops|BUG:|rcu.*stall|soft lockup|hard LOCKUP" | sed "s/^/ERROS-/"; echo SMP-""DONE'
(
    t=0; until grep -a -q "login:" $log 2>/dev/null || [ $t -ge $((TMO - 200)) ]; do sleep 2; t=$((t + 2)); done
    sleep 2; printf 'root\n'; sleep 5
    printf '%s\n' "$CMDS"
    # espera o fim (a VM sai sozinha com -e) em vez de um tempo fixo
    until grep -a -q "^SMP-DONE" $log 2>/dev/null || [ $t -ge $TMO ]; do sleep 2; t=$((t + 2)); done
) | "$MVM" -a $ARCH -m 1024 --smp $N -r "$ISO" --bios "$BIOS" --net none -t $TMO -e SMP-DONE > $log 2>&1
L=$(tr -d '\r' < $log)
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FALHA $1"; fail=1; fi; }
check "kernel ativou $N CPUs" "echo \"\$L\" | grep -q 'SMP: Total of $N processors activated'"
check "nproc = $N" "echo \"\$L\" | grep -q '^NPROC-$N\$'"
check "5 md5 iguais (1 + 4 em paralelo)" "echo \"\$L\" | grep -q '^MD5S-1\$'"
check "300 processos em paralelo" "echo \"\$L\" | grep -q '^FORK-OK'"
check "sem Oops/BUG/travamento no kernel" "echo \"\$L\" | grep -q '^ERROS-0\$'"
echo "tempo: $(echo "$L" | grep -o '^UMA-[0-9]*s') / $(echo "$L" | grep -o '^QUATRO-[0-9]*s') (4 tarefas em paralelo)"
[ $fail = 0 ] && echo "smp $ARCH: tudo ok" || echo "smp $ARCH: houve falhas (log em tests/smp/$log)"
exit $fail
