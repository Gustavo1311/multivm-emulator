#!/bin/sh
# Boota as ISOs de instalacao do OpenBSD (amd64 e i386), NetBSD e FreeBSD com BIOS e
# espera o instalador aparecer na tela de texto (lida pelo FIFO de controle, comando "text").
# FreeBSD e NetBSD tambem com 2 CPUs (os kernels de instalacao sao SMP).
# Execute antes: ./fetch_isos.sh  (ou BSD_DIR=pasta com as ISOs)
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
BIOS=${BIOS:-/usr/share/seabios/bios-256k.bin}
D=${BSD_DIR:-images}
OUT=out
mkdir -p $OUT
fail=0

# nome, arquitetura, ISO, texto esperado, limite (s), opcoes extras
run() {
    name=$1 arch=$2 iso=$D/$3 want=$4 lim=$5; shift 5
    if [ ! -s "$iso" ]; then echo "pula  $name (sem $iso)"; return; fi
    fifo=$OUT/$name.fifo scr=$OUT/$name.txt
    rm -f $fifo $scr; mkfifo $fifo
    truncate -s 2G $OUT/$name.img
    t0=$(date +%s)
    "$MVM" -a $arch -m 1024 --bios "$BIOS" --cdrom "$iso" -d $OUT/$name.img --boot d --net e1000 --audio hda \
        --control $fifo -t $((lim + 30)) "$@" > $OUT/$name.log 2>&1 < /dev/null &
    pid=$!
    ok=FALHOU
    while kill -0 $pid 2>/dev/null && [ $(($(date +%s) - t0)) -lt $lim ]; do
        sleep 5
        rm -f $scr
        timeout 5 sh -c "echo 'text $scr' > $fifo" || continue
        sleep 1
        if grep -q -- "$want" $scr 2>/dev/null; then ok=ok; break; fi
    done
    timeout 5 sh -c "echo quit > $fifo"
    sleep 2; kill $pid 2>/dev/null; wait $pid 2>/dev/null
    printf "%-16s %-7s %4ss\n" $name $ok $(($(date +%s) - t0))
    [ $ok = ok ] || { fail=1; echo "  tela:"; tail -5 $scr 2>/dev/null; }
    rm -f $fifo $OUT/$name.img
}

run openbsd-amd64  x86_64 openbsd79-amd64-install.iso "(I)nstall, (U)pgrade" 400
run openbsd-i386   i386   openbsd79-i386-cd.iso       "(I)nstall, (U)pgrade" 400
run netbsd         x86_64 NetBSD-11.0-amd64.iso       "Installation messages in English" 600
run netbsd-smp2    x86_64 NetBSD-11.0-amd64.iso       "Installation messages in English" 600 --smp 2
run freebsd        x86_64 FreeBSD-15.1-RELEASE-amd64-disc1.iso "Live System" 600
run freebsd-smp2   x86_64 FreeBSD-15.1-RELEASE-amd64-disc1.iso "Live System" 600 --smp 2
exit $fail
