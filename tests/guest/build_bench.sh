#!/bin/sh
# Gera out/bench-x86_64.elf, out/bench-i386.elf e out/bench-arm64.elf (testes de CPU repetidos).
#   BENCH_N=200 ./build_bench.sh
set -e
cd "$(dirname "$0")"
./build.sh > /dev/null
N=${BENCH_N:-200}
CLANG=${CLANG:-clang}; LLD=${LLD:-ld.lld}; OUT=out
CFLAGS="-O2 -ffp-contract=off -ffreestanding -fno-math-errno -nostdlib -fno-stack-protector -fno-pic -Wall"
$CLANG --target=x86_64-none-elf $CFLAGS -O3 -mno-red-zone -Dtest_main=test_main_once -c common/test.c -o $OUT/bx64_test.o
$CLANG --target=x86_64-none-elf $CFLAGS -mno-red-zone -DBENCH_N=$N -c common/bench.c -o $OUT/bx64_bench.o
$LLD -m elf_x86_64 -T common/link.ld --defsym=LOAD_ADDR=0x200000 -o $OUT/bench-x86_64.elf \
    $OUT/x64_start.o $OUT/bx64_test.o $OUT/bx64_bench.o $OUT/x64_sys.o $OUT/x64_libc.o $OUT/x64_rt.o
$CLANG --target=i386-none-elf $CFLAGS -O3 -msse2 -Dtest_main=test_main_once -c common/test.c -o $OUT/bx32_test.o
$CLANG --target=i386-none-elf $CFLAGS -DBENCH_N=$N -c common/bench.c -o $OUT/bx32_bench.o
$LLD -m elf_i386 -T common/link.ld --defsym=LOAD_ADDR=0x200000 -o $OUT/bench-i386.elf \
    $OUT/x32_start.o $OUT/bx32_test.o $OUT/bx32_bench.o $OUT/x32_sys.o $OUT/x32_libc.o $OUT/x32_rt.o
$CLANG --target=aarch64-none-elf $CFLAGS -O3 -Dtest_main=test_main_once -c common/test.c -o $OUT/ba64_test.o
$CLANG --target=aarch64-none-elf $CFLAGS -DBENCH_N=$N -c common/bench.c -o $OUT/ba64_bench.o
$LLD -m aarch64elf -T common/link.ld --defsym=LOAD_ADDR=0x40080000 -o $OUT/bench-arm64.elf \
    $OUT/a64_start.o $OUT/ba64_test.o $OUT/ba64_bench.o $OUT/a64_sys.o $OUT/a64_vectors.o $OUT/a64_libc.o
echo ok
