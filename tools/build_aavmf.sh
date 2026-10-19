#!/bin/bash
# Compila o firmware UEFI das VMs ARM (edk2 ArmVirtQemu) embutido no app:
#   android/app/src/main/assets/bios/AAVMF_CODE.fd   + AAVMF_VARS.fd    (ARM64)
#   android/app/src/main/assets/bios/AAVMF32_CODE.fd + AAVMF32_VARS.fd  (ARM 32 bits)
# Mesmo clone do edk2 que o tools/build_ovmf.sh:
#   apt install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf iasl uuid-dev python3
#   git clone --branch edk2-stable202502 --recursive https://github.com/tianocore/edk2 ~/src/edk2
#   tools/build_aavmf.sh [RELEASE|DEBUG] [AARCH64|ARM|ALL]   (DEBUG escreve o log na PL011)
# Aplica tools/edk2-patches/*.patch (tela virtio-gpu e teclado virtio por MMIO no console do firmware).
set -e
EDK2=${EDK2:-$HOME/src/edk2}
OUT=$(cd "$(dirname "$0")/.." && pwd)/android/app/src/main/assets/bios
PATCHES=$(cd "$(dirname "$0")" && pwd)/edk2-patches
TYPE=${1:-RELEASE}
WHICH=${2:-ALL}
cd "$EDK2"
[ -x BaseTools/Source/C/bin/GenFw ] || make -C BaseTools -j"$(nproc)"
for p in "$PATCHES"/*.patch; do
    git apply --check -R "$p" 2>/dev/null || git apply "$p"
done
A64=
[ "$(uname -m)" = aarch64 ] || A64=aarch64-linux-gnu-
export GCC5_AARCH64_PREFIX=$A64 GCC_AARCH64_PREFIX=$A64
export GCC5_ARM_PREFIX=arm-linux-gnueabihf- GCC_ARM_PREFIX=arm-linux-gnueabihf-
set +e
source edksetup.sh BaseTools > /dev/null
set -e
build_one() { # ARCH NOME_SAIDA
    # sem Secure Boot/TPM; rede so IPv4 + PXE (padrao do ArmVirtQemu)
    build -a "$1" -t GCC5 -p ArmVirtPkg/ArmVirtQemu.dsc -b "$TYPE" -n "$(nproc)" \
        -D NETWORK_TLS_ENABLE=FALSE -D NETWORK_HTTP_BOOT_ENABLE=FALSE -D NETWORK_IP6_ENABLE=FALSE
    FV=Build/ArmVirtQemu-$1/${TYPE}_GCC5/FV
    if [ "$TYPE" = RELEASE ]; then
        cp "$FV/QEMU_EFI.fd" "$OUT/$2_CODE.fd"
        cp "$FV/QEMU_VARS.fd" "$OUT/$2_VARS.fd"
        echo "copiado para $OUT/$2_CODE.fd e $2_VARS.fd"
    else
        echo "imagens em $EDK2/$FV (QEMU_EFI.fd e QEMU_VARS.fd)"
    fi
}
case "$WHICH" in
AARCH64) build_one AARCH64 AAVMF ;;
ARM) build_one ARM AAVMF32 ;;
*) build_one AARCH64 AAVMF; build_one ARM AAVMF32 ;;
esac
