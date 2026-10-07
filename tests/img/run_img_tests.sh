#!/bin/sh
# Testa os formatos de imagem de disco do MultiVM contra o qemu-img (referencia):
# qcow2, VDI, VMDK, VHD, VHDX e o formato proprio MVD.
#   run_img_tests.sh [N_ESCRITAS]
cd "$(dirname "$0")"
N=${1:-3000}
CORE=../../core
IMG=$CORE/build/mvm-img
W=out
mkdir -p $W
cc -O2 -I$CORE/src -I$CORE/include imgtest.c $CORE/build/libmvmcore.a -lpthread -lm -o $W/imgtest || exit 1
T=$W/imgtest
# imagem raw de 64 MiB com regioes de dados, zeros e buracos
python3 - <<'PY'
import os, random
random.seed(7)
f = open('out/base.raw', 'wb'); f.truncate(64 << 20)
for i in range(400):
    off = random.randrange(0, (64 << 20) - 200000) & ~511
    n = random.randrange(1, 200000)
    f.seek(off); f.write(os.urandom(n) if random.random() < 0.7 else bytes(n))
f.close()
PY
fail=0
ok() { echo "$1: ok"; }
bad() { echo "$1: FALHOU ($2)"; fail=1; }
seed() { echo "0x$(echo "$1" | md5sum | cut -c1-8)"; }

# leitura igual a do qemu-img; escritas aleatorias; qemu-img check (se houver) e compare
# fmt_test NOME FORMATO_QEMU ARQ [check] [somente-leitura]
fmt_test() {
    name=$1; fmt=$2; f=$3; chk=$4; ro=$5
    qemu-img convert -q -f $fmt -O raw $f $W/ref.raw || { bad "$name" "qemu-img nao leu"; return; }
    $T read $f $W/out.raw > $W/msg.txt || { bad "$name" "$(cat $W/msg.txt)"; return; }
    cmp -s $W/out.raw $W/ref.raw || { bad "$name" "leitura difere do qemu-img"; return; }
    if [ "$ro" = ro ]; then
        $T write $f $W/ref.raw 1 1 > $W/msg.txt 2>&1 && { bad "$name" "aceitou escrita"; return; }
        ok "$name (leitura)"; return
    fi
    cp $W/ref.raw $W/mirror.raw
    $T write $f $W/mirror.raw $(seed $name) $N > $W/msg.txt || { bad "$name" "escrita: $(cat $W/msg.txt)"; return; }
    if [ "$chk" = check ] && ! qemu-img check -q -f $fmt $f > $W/chk.txt 2>&1; then bad "$name" "qemu-img check: $(tail -2 $W/chk.txt)"; return; fi
    qemu-img compare -q -f $fmt -F raw $f $W/mirror.raw || { bad "$name" "conteudo apos escritas difere"; return; }
    ok "$name"
}

conv() { rm -f $W/t.$2; qemu-img convert -q -f raw -O $1 $3 $W/base.raw $W/t.$2; }

echo "== qcow2"
for v in "padrao:" "v2:-o compat=0.10" "c4k:-o cluster_size=4096" "c2m:-o cluster_size=2M" "comprimida:-c" \
         "rc1:-o refcount_bits=1" "rc64:-o refcount_bits=64" "c512:-o cluster_size=512"; do
    name=${v%%:*}; opts=${v#*:}
    conv qcow2 qcow2 "$opts"
    fmt_test "qcow2 $name" qcow2 $W/t.qcow2 check
done
conv qcow2 qcow2 ""; qemu-img snapshot -c antes $W/t.qcow2
fmt_test "qcow2 snapshot (estado ativo)" qcow2 $W/t.qcow2 check
qemu-img snapshot -a antes $W/t.qcow2
qemu-img compare -q -f qcow2 -F raw $W/t.qcow2 $W/base.raw && ok "qcow2 snapshot (intacto)" || bad "qcow2 snapshot" "snapshot alterado"
rm -f $W/filho.qcow2; conv qcow2 qcow2 ""; qemu-img create -q -f qcow2 -b t.qcow2 -F qcow2 $W/filho.qcow2
cp $W/t.qcow2 $W/pai-antes.qcow2
fmt_test "qcow2 com arquivo base" qcow2 $W/filho.qcow2 check
cmp -s $W/t.qcow2 $W/pai-antes.qcow2 && ok "qcow2 arquivo base intacto" || bad "qcow2 arquivo base" "pai alterado"

echo "== VDI"
conv vdi vdi ""; fmt_test "vdi dinamico" vdi $W/t.vdi check
conv vdi vdi "-o static=on"; fmt_test "vdi fixo" vdi $W/t.vdi check

echo "== VMDK"
for sub in monolithicSparse monolithicFlat twoGbMaxExtentSparse twoGbMaxExtentFlat; do
    rm -f $W/t*.vmdk; conv vmdk vmdk "-o subformat=$sub"
    fmt_test "vmdk $sub" vmdk $W/t.vmdk check
done
rm -f $W/t*.vmdk; conv vmdk vmdk "-o subformat=streamOptimized"
fmt_test "vmdk streamOptimized" vmdk $W/t.vmdk "" ro
rm -f $W/t*.vmdk $W/f*.vmdk; conv vmdk vmdk ""; qemu-img create -q -f vmdk -b t.vmdk -F vmdk $W/filho.vmdk
$T open $W/filho.vmdk | grep -q "parentCID" && ok "vmdk filho recusado" || bad "vmdk filho" "nao recusou"

echo "== VHD"
conv vpc vhd ""; fmt_test "vhd dinamico" vpc $W/t.vhd
conv vpc vhd "-o subformat=fixed"; fmt_test "vhd fixo" vpc $W/t.vhd
conv vpc vhd "-o force_size=on"; fmt_test "vhd dinamico (force_size)" vpc $W/t.vhd
# diferencial (gerado aqui; o qemu-img nao cria): conferido pelas nossas leituras
conv vpc vhd ""; mv $W/t.vhd $W/pai.vhd; qemu-img convert -q -f vpc -O raw $W/pai.vhd $W/pai.raw
python3 mkimages.py vhddiff $W/pai.vhd $W/filho.vhd $W/pai.raw $W/esperado.raw
cp $W/pai.vhd $W/pai-antes.vhd
if $T read $W/filho.vhd $W/out.raw > $W/msg.txt && cmp -s $W/out.raw $W/esperado.raw; then
    cp $W/esperado.raw $W/mirror.raw
    if $T write $W/filho.vhd $W/mirror.raw 3 $N > $W/msg.txt && $T read $W/filho.vhd $W/out.raw > /dev/null &&
       cmp -s $W/out.raw $W/mirror.raw && cmp -s $W/pai.vhd $W/pai-antes.vhd; then ok "vhd diferencial"
    else bad "vhd diferencial" "escrita: $(cat $W/msg.txt)"; fi
else bad "vhd diferencial" "leitura: $(cat $W/msg.txt)"; fi

echo "== VHDX"
conv vhdx vhdx ""; fmt_test "vhdx dinamico" vhdx $W/t.vhdx check
conv vhdx vhdx "-o subformat=fixed"; fmt_test "vhdx fixo" vhdx $W/t.vhdx check
conv vhdx vhdx "-o block_size=1M"; fmt_test "vhdx blocos 1M" vhdx $W/t.vhdx check
conv vhdx vhdx "-o block_size=32M"; fmt_test "vhdx blocos 32M" vhdx $W/t.vhdx check
# log pendente: reaplicado na memoria (somente leitura) e no arquivo (gravavel)
conv vhdx vhdx "-o block_size=1M"; qemu-img convert -q -f vhdx -O raw $W/t.vhdx $W/atual.raw
VHDX_BLOCK=1048576 python3 mkimages.py vhdxlog $W/t.vhdx $W/atual.raw $W/esperado.raw
h0=$(md5sum < $W/t.vhdx)
if $T read $W/t.vhdx $W/out.raw > $W/msg.txt 2>&1 && cmp -s $W/out.raw $W/esperado.raw && [ "$h0" = "$(md5sum < $W/t.vhdx)" ]; then
    ok "vhdx log (memoria)"
else bad "vhdx log (memoria)" "$(cat $W/msg.txt)"; fi
cp $W/esperado.raw $W/mirror.raw
if $T write $W/t.vhdx $W/mirror.raw 4 300 > $W/msg.txt 2>&1 && qemu-img check -q -f vhdx $W/t.vhdx &&
   qemu-img compare -q -f vhdx -F raw $W/t.vhdx $W/mirror.raw; then ok "vhdx log (arquivo)"
else bad "vhdx log (arquivo)" "$(cat $W/msg.txt)"; fi

echo "== MVD"
mvd_test() { # nome opcoes-do-convert
    name=$1; shift
    rm -f $W/t.mvd
    $IMG convert -O mvd "$@" $W/base.raw $W/t.mvd 2> $W/msg.txt || { bad "$name" "convert: $(cat $W/msg.txt)"; return; }
    $T read $W/t.mvd $W/out.raw > /dev/null && cmp -s $W/out.raw $W/base.raw || { bad "$name" "leitura"; return; }
    cp $W/base.raw $W/mirror.raw
    $T write $W/t.mvd $W/mirror.raw $(seed $name) $N discard > $W/msg.txt || { bad "$name" "escrita: $(cat $W/msg.txt)"; return; }
    $IMG check $W/t.mvd > $W/chk.txt || { bad "$name" "check: $(cat $W/chk.txt)"; return; }
    $T read $W/t.mvd $W/out.raw > /dev/null && cmp -s $W/out.raw $W/mirror.raw || { bad "$name" "conteudo apos escritas"; return; }
    $IMG convert -O raw $W/t.mvd $W/volta.raw 2>/dev/null && cmp -s $W/volta.raw $W/mirror.raw || { bad "$name" "conversao de volta"; return; }
    $IMG compact $W/t.mvd 2>/dev/null && $T read $W/t.mvd $W/out.raw > /dev/null && cmp -s $W/out.raw $W/mirror.raw || { bad "$name" "compact"; return; }
    ok "$name"
}
mvd_test "mvd padrao"
mvd_test "mvd blocos 64K" -b 64K
mvd_test "mvd blocos 4M" -b 4M
mvd_test "mvd comprimido" -c
# conversoes a partir de outros formatos
for src in "qcow2:-c" "vdi:" "vmdk:-o subformat=streamOptimized" "vpc:-o force_size=on" "vhdx:"; do
    f=${src%%:*}; o=${src#*:}
    rm -f $W/s.img $W/s-*.vmdk; qemu-img convert -q -f raw -O $f $o $W/base.raw $W/s.img
    qemu-img convert -q -f $f -O raw $W/s.img $W/ref.raw
    rm -f $W/t.mvd
    if $IMG convert -c $W/s.img $W/t.mvd 2>/dev/null && $T read $W/t.mvd $W/out.raw >/dev/null && cmp -s $W/out.raw $W/ref.raw; then
        ok "convert $f -> mvd"
    else bad "convert $f -> mvd" "conteudo"; fi
done
# overlay (snapshot) sobre um qcow2: o base nao muda
conv qcow2 qcow2 ""; cp $W/t.qcow2 $W/pai-antes.qcow2; rm -f $W/over.mvd
$IMG snapshot $W/t.qcow2 $W/over.mvd && cp $W/base.raw $W/mirror.raw &&
$T write $W/over.mvd $W/mirror.raw 8 $N discard > $W/msg.txt && $T read $W/over.mvd $W/out.raw > /dev/null &&
cmp -s $W/out.raw $W/mirror.raw && cmp -s $W/t.qcow2 $W/pai-antes.qcow2 && ok "mvd overlay sobre qcow2" || bad "mvd overlay" "$(cat $W/msg.txt)"
# disco grande vazio: arquivo pequeno; TRIM devolve espaco para reuso
rm -f $W/g.mvd; $IMG create $W/g.mvd 128G
s0=$(stat -c %s $W/g.mvd)
truncate -s 0 $W/mirror.raw; truncate -s 128G $W/mirror.raw
$T write $W/g.mvd $W/mirror.raw 5 300 discard > $W/msg.txt && ok "mvd 128G (arquivo vazio: $((s0 / 1024)) KiB, depois $(( $(stat -c %s $W/g.mvd) / 1048576)) MiB)" || bad "mvd 128G" "$(cat $W/msg.txt)"
rm -f $W/g.mvd $W/mirror.raw
# queda: o que nao foi confirmado por flush pode se perder, mas nada alem disso
for s in 11 22 33 44 55; do
    rm -f $W/t.mvd; $IMG convert $W/base.raw $W/t.mvd 2>/dev/null
    cp $W/base.raw $W/commit.raw; truncate -s 0 $W/touched.raw; truncate -s 64M $W/touched.raw
    $T crash $W/t.mvd $W/commit.raw $W/touched.raw $s 1500 > $W/msg.txt || { bad "mvd queda $s" "$(cat $W/msg.txt)"; continue; }
    $IMG check $W/t.mvd > $W/chk.txt || { bad "mvd queda $s" "check: $(cat $W/chk.txt)"; continue; }
    $T verify2 $W/t.mvd $W/commit.raw $W/touched.raw > $W/msg.txt && ok "mvd queda $s ($(cat $W/msg.txt))" || bad "mvd queda $s" "$(cat $W/msg.txt)"
done

echo "== protecao de imagens raw"
cp $W/base.raw $W/g.raw; $T guard $W/g.raw > /dev/null && ok "assinaturas de formato bloqueadas em raw" || bad "protecao raw" "gravou"
rm -f $W/g.raw
exit $fail
