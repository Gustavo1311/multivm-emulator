#!/bin/sh
# Gera multivm/src/main/jniLibs/<abi>/libmultivm.so sem o Android NDK, usando um
# clang que ja tenha como alvo o Android (ex.: o clang do Termux, que roda no proprio
# aparelho). Com o NDK instalado, prefira o build via Gradle/CMake (multivm.nativeBuild=cmake).
#
#   ./build-native.sh                 # arm64-v8a
#   CLANG=/caminho/clang ./build-native.sh
set -e
cd "$(dirname "$0")"
CLANG=${CLANG:-clang}
ABI=arm64-v8a
TARGET=aarch64-linux-android24
CORE=../core
OUT=multivm/src/main/jniLibs/$ABI
OBJ=build-native/$ABI
mkdir -p "$OUT" "$OBJ"
JNI_INC=${JNI_INC:-}
if [ -z "$JNI_INC" ]; then
    for d in /usr/lib/jvm/default-java/include /usr/lib/jvm/*/include "$PREFIX/include"; do
        [ -f "$d/jni.h" ] && JNI_INC="-I$d -I$d/linux" && break
    done
fi
CFLAGS="--target=$TARGET -O3 -fPIC -fvisibility=hidden -Wall -Wno-unused-parameter -fno-strict-aliasing -ffp-contract=off -I$CORE/include"
SRCS="$CORE/src/space.c $CORE/src/util.c $CORE/src/img/img_util.c $CORE/src/img/qcow2.c $CORE/src/img/vdi.c $CORE/src/img/vmdk.c $CORE/src/img/vhd.c $CORE/src/img/vhdx.c $CORE/src/img/mvd.c $CORE/src/img/lz4.c $CORE/src/img/convert.c $CORE/src/inflate.c $CORE/src/vm.c $CORE/src/loader.c
      $CORE/src/cpu/arm_common.c $CORE/src/cpu/cpu_arm64.c $CORE/src/cpu/a64_simd.c
      $CORE/src/cpu/cpu_arm32.c $CORE/src/cpu/a32_thumb.c $CORE/src/cpu/a32_vfp.c
      $CORE/src/cpu/cpu_x86.c $CORE/src/cpu/x86_exec.c $CORE/src/cpu/x86_fpu.c $CORE/src/cpu/x86_sse.c $CORE/src/cpu/x86_jit.c
      $CORE/src/dev/virtio.c $CORE/src/dev/arm_devices.c $CORE/src/dev/pc_devices.c $CORE/src/dev/pci.c
      $CORE/src/dev/fw_cfg.c $CORE/src/dev/vga.c $CORE/src/dev/ide.c $CORE/src/dev/i8237.c $CORE/src/dev/fdc.c $CORE/src/dev/apic.c $CORE/src/dev/acpi.c $CORE/src/dev/hpet.c $CORE/src/dev/ahci.c
      $CORE/src/dev/eeprom93.c $CORE/src/dev/virtio_net.c $CORE/src/dev/virtio_snd.c $CORE/src/dev/rtl8139.c $CORE/src/dev/e1000.c
      $CORE/src/dev/ac97.c $CORE/src/dev/hda.c $CORE/src/net/net.c $CORE/src/net/tcp.c $CORE/src/audio.c $CORE/src/vnc.c $CORE/src/des.c $CORE/src/zlib_dl.c $CORE/src/jpeg_enc.c
      $CORE/src/machine/machine_virt.c $CORE/src/machine/machine_pc.c"
OBJS=""
for s in $SRCS; do
    o=$OBJ/$(basename "$s" .c).o
    $CLANG $CFLAGS -c "$s" -o "$o"
    OBJS="$OBJS $o"
done
# a ponte JNI exporta os simbolos Java_* (visibilidade padrao)
$CLANG --target=$TARGET -O2 -fPIC -Wall -Wno-unused-parameter -I$CORE/include $JNI_INC \
    -c multivm/src/main/cpp/jni_bridge.c -o $OBJ/jni_bridge.o
$CLANG $CFLAGS -c multivm/src/main/cpp/audio_aaudio.c -o $OBJ/audio_aaudio.o
$CLANG --target=$TARGET -shared -o "$OUT/libmultivm.so" $OBJS $OBJ/jni_bridge.o $OBJ/audio_aaudio.o \
    -Wl,-soname,libmultivm.so -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--no-undefined \
    -Wl,--disable-new-dtags -llog -lm -ldl
command -v python3 > /dev/null && python3 strip_rpath.py "$OUT/libmultivm.so"
echo "gerado: $OUT/libmultivm.so"
