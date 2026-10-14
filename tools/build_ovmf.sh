#!/bin/bash
# Compila o firmware UEFI (OVMF X64) embutido no app: android/app/src/main/assets/bios/OVMF_CODE.fd
# e OVMF_VARS.fd. Funciona tambem num host ARM64 (compilador cruzado x86_64-linux-gnu-gcc).
#   apt install gcc-x86-64-linux-gnu nasm iasl uuid-dev python3
#   git clone --branch edk2-stable202502 --recursive https://github.com/tianocore/edk2 ~/src/edk2
#   tools/build_ovmf.sh [RELEASE|DEBUG]   (~30 min; DEBUG escreve o log na porta 0x402)
set -e
EDK2=${EDK2:-$HOME/src/edk2}
OUT=$(cd "$(dirname "$0")/.." && pwd)/android/app/src/main/assets/bios
TYPE=${1:-RELEASE}
cd "$EDK2"
[ -x BaseTools/Source/C/bin/GenFw ] || make -C BaseTools -j"$(nproc)"
CROSS=
[ "$(uname -m)" = x86_64 ] || CROSS=x86_64-linux-gnu-
export GCC5_BIN=$CROSS GCC_BIN=$CROSS
set +e
source edksetup.sh BaseTools > /dev/null
set -e
# sem SMM/Secure Boot/TPM (a maquina e um i440FX sem SMM); sem TLS/HTTP boot para ficar menor
build -a X64 -t GCC5 -p OvmfPkg/OvmfPkgX64.dsc -b "$TYPE" -n "$(nproc)" \
    -D NETWORK_TLS_ENABLE=FALSE -D NETWORK_HTTP_BOOT_ENABLE=FALSE -D NETWORK_IP6_ENABLE=FALSE \
    -D TPM2_ENABLE=FALSE -D FD_SIZE_4MB=TRUE
FV=Build/OvmfX64/${TYPE}_GCC5/FV
if [ "$TYPE" = RELEASE ]; then
    cp "$FV/OVMF_CODE.fd" "$FV/OVMF_VARS.fd" "$OUT/"
    echo "copiado para $OUT"
else
    echo "imagens em $EDK2/$FV (OVMF.fd = codigo + variaveis)"
fi
