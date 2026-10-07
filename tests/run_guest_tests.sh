#!/bin/sh
# Compila e executa os programas de teste bare-metal nas 4 arquiteturas e compara
# a saida do teste de CPU com a referencia nativa. Requer clang multi-alvo e ld.lld.
#   MVM=/caminho/mvm-cli ./run_guest_tests.sh
set -e
cd "$(dirname "$0")/guest"
MVM=${MVM:-../../core/build/mvm-cli}
./build.sh > /dev/null
./out/test-native > out/expected.txt
fail=0
for t in arm64:arm64 arm:arm arm:thumb x86_64:x86_64 i386:i386; do
    a=${t%%:*}; e=${t##*:}
    "$MVM" -a "$a" -k "out/$e.elf" --fb 320x200 -t 60 < /dev/null > "out/run-$e.txt" 2>/dev/null || true
    cpu=FALHOU; head -10 "out/run-$e.txt" | diff -q - out/expected.txt > /dev/null && cpu=ok
    sys=FALHOU; grep -q "SYS OK" "out/run-$e.txt" && sys=ok
    printf "%-8s cpu:%-7s sistema:%-7s (%s verificacoes)\n" "$e" "$cpu" "$sys" "$(grep -c '  ok' "out/run-$e.txt")"
    [ "$cpu" = ok ] && [ "$sys" = ok ] || fail=1
done
exit $fail
