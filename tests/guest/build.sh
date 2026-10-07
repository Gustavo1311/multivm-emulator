#!/bin/sh
# Compila os programas de teste convidados para cada arquitetura (usa clang multi-alvo)
# e a referencia nativa. Saida em tests/guest/out/.
set -e
cd "$(dirname "$0")"
CLANG=${CLANG:-clang}
LLD=${LLD:-ld.lld}
OUT=out
mkdir -p $OUT
CFLAGS="-O2 -ffp-contract=off -ffreestanding -fno-math-errno -nostdlib -fno-stack-protector -fno-pic -Wall"
LIBC_FLAGS="-O2 -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fno-pic"

# referencia nativa
${HOSTCC:-cc} -O2 -ffp-contract=off -o $OUT/test-native common/test.c common/native_main.c

# arm64
$CLANG --target=aarch64-none-elf $CFLAGS -O3 -c common/test.c -o $OUT/a64_test.o
$CLANG --target=aarch64-none-elf $CFLAGS -c arm64/sys.c -o $OUT/a64_sys.o
$CLANG --target=aarch64-none-elf $LIBC_FLAGS -c common/libc.c -o $OUT/a64_libc.o
$CLANG --target=aarch64-none-elf -c arm64/start.S -o $OUT/a64_start.o
$CLANG --target=aarch64-none-elf -c arm64/vectors.S -o $OUT/a64_vectors.o
$LLD -m aarch64elf -T common/link.ld --defsym=LOAD_ADDR=0x40080000 -o $OUT/arm64.elf \
    $OUT/a64_start.o $OUT/a64_test.o $OUT/a64_sys.o $OUT/a64_vectors.o $OUT/a64_libc.o

if [ -f arm/start.S ]; then
    $CLANG --target=armv7a-none-eabihf -mcpu=cortex-a15 -mfpu=vfpv3-d16 -marm $CFLAGS -O3 -c common/test.c -o $OUT/a32_test.o
    $CLANG --target=armv7a-none-eabihf -mcpu=cortex-a15 -mfpu=vfpv3-d16 -mthumb $CFLAGS -O3 -c common/test.c -o $OUT/t32_test.o
    $CLANG --target=armv7a-none-eabihf -mcpu=cortex-a15 -mfpu=vfpv3-d16 -marm $CFLAGS -c arm/sys.c -o $OUT/a32_sys.o
    $CLANG --target=armv7a-none-eabihf -mcpu=cortex-a15 -mfpu=vfpv3-d16 -marm $LIBC_FLAGS -c common/libc.c -o $OUT/a32_libc.o
    $CLANG --target=armv7a-none-eabihf -mcpu=cortex-a15 -mfpu=vfpv3-d16 -marm $LIBC_FLAGS -c common/rt32.c -o $OUT/a32_rt.o
    $CLANG --target=armv7a-none-eabihf -mcpu=cortex-a15 -mfpu=vfpv3-d16 -mthumb $CFLAGS -c arm/sys.c -o $OUT/t32_sys.o
    $CLANG --target=armv7a-none-eabihf -mcpu=cortex-a15 -mfpu=vfpv3-d16 -c arm/start.S -o $OUT/a32_start.o
    $LLD -m armelf -T common/link.ld --defsym=LOAD_ADDR=0x40010000 -o $OUT/arm.elf \
        $OUT/a32_start.o $OUT/a32_test.o $OUT/a32_sys.o $OUT/a32_libc.o $OUT/a32_rt.o
    $LLD -m armelf -T common/link.ld --defsym=LOAD_ADDR=0x40010000 -o $OUT/thumb.elf \
        $OUT/a32_start.o $OUT/t32_test.o $OUT/t32_sys.o $OUT/a32_libc.o $OUT/a32_rt.o
fi

if [ -f x86_64/start.S ]; then
    $CLANG --target=x86_64-none-elf $CFLAGS -O3 -mno-red-zone -c common/test.c -o $OUT/x64_test.o
    $CLANG --target=x86_64-none-elf $CFLAGS -mno-red-zone -mgeneral-regs-only -c x86_64/sys.c -o $OUT/x64_sys.o
    $CLANG --target=x86_64-none-elf $LIBC_FLAGS -mno-red-zone -c common/libc.c -o $OUT/x64_libc.o
    $CLANG --target=x86_64-none-elf $LIBC_FLAGS -mno-red-zone -c common/rt32.c -o $OUT/x64_rt.o
    $CLANG --target=x86_64-none-elf -c x86_64/start.S -o $OUT/x64_start.o
    $LLD -m elf_x86_64 -T common/link.ld --defsym=LOAD_ADDR=0x200000 -o $OUT/x86_64.elf \
        $OUT/x64_start.o $OUT/x64_test.o $OUT/x64_sys.o $OUT/x64_libc.o $OUT/x64_rt.o
fi

if [ -f i386/start.S ]; then
    $CLANG --target=i386-none-elf $CFLAGS -O3 -msse2 -c common/test.c -o $OUT/x32_test.o
    $CLANG --target=i386-none-elf $CFLAGS -O2 -mno-sse -mno-mmx -c common/test.c -o $OUT/x32_test_x87.o
    $CLANG --target=i386-none-elf $CFLAGS -mno-sse -mno-mmx -c i386/sys.c -o $OUT/x32_sys.o
    $CLANG --target=i386-none-elf $LIBC_FLAGS -c common/libc.c -o $OUT/x32_libc.o
    $CLANG --target=i386-none-elf $LIBC_FLAGS -c common/rt32.c -o $OUT/x32_rt.o
    $CLANG --target=i386-none-elf -c i386/start.S -o $OUT/x32_start.o
    $LLD -m elf_i386 -T common/link.ld --defsym=LOAD_ADDR=0x200000 -o $OUT/i386.elf \
        $OUT/x32_start.o $OUT/x32_test.o $OUT/x32_sys.o $OUT/x32_libc.o $OUT/x32_rt.o
    $LLD -m elf_i386 -T common/link.ld --defsym=LOAD_ADDR=0x200000 -o $OUT/i386_x87.elf \
        $OUT/x32_start.o $OUT/x32_test_x87.o $OUT/x32_sys.o $OUT/x32_libc.o $OUT/x32_rt.o
fi
echo "ok"
