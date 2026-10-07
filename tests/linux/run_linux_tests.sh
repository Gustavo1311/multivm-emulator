#!/bin/sh
# Boota o Linux (Alpine) nas 4 arquiteturas com um disco virtio-blk e verifica:
# boot ate o userland, leitura do disco (md5 igual ao do host) e escrita persistida.
# Com qemu-img no host, repete em x86_64 e arm64 com o disco em qcow2 (qemu-img check no fim).
# Repete tambem com o disco em MVD (formato proprio), que anuncia TRIM: o convidado descarta
# 4 MiB (blkdiscard), a faixa tem de ler zerada e os blocos voltam a ser livres no arquivo.
# Execute antes: ./fetch_images.sh
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
fail=0
mkdir -p images/tmp
dd if=/dev/urandom of=images/tmp/base.img bs=1M count=16 2>/dev/null
want=$(dd if=images/tmp/base.img bs=1k count=16 2>/dev/null | md5sum | cut -d' ' -f1)
specs="arm64:aarch64:ttyAMA0 arm:armv7:ttyAMA0 x86_64:x86_64:ttyS0 i386:x86:ttyS0"
command -v qemu-img >/dev/null && specs="$specs arm64:aarch64:ttyAMA0:qcow2 x86_64:x86_64:ttyS0:qcow2"
specs="$specs arm64:aarch64:ttyAMA0:mvd x86_64:x86_64:ttyS0:mvd arm:armv7:ttyAMA0:mvd i386:x86:ttyS0:mvd"
IMG=${IMG:-../../core/build/mvm-img}
for spec in $specs; do
    arch=${spec%%:*}; rest=${spec#*:}; dir=${rest%%:*}; con=${rest#*:}; fmt=raw
    case "$con" in *:qcow2) con=${con%:qcow2}; fmt=qcow2 ;; *:mvd) con=${con%:mvd}; fmt=mvd ;; esac
    img=images/tmp/$arch.img
    if [ $fmt = qcow2 ]; then
        img=images/tmp/$arch.qcow2
        qemu-img convert -f raw -O qcow2 images/tmp/base.img $img
    elif [ $fmt = mvd ]; then
        img=images/tmp/$arch.mvd
        rm -f $img
        "$IMG" convert images/tmp/base.img $img 2>/dev/null
    else
        cp images/tmp/base.img $img
    fi
    t0=$(date +%s)
    out=$("$MVM" -a $arch -m 256 -k images/$dir/vmlinuz -i images/$dir/disktest.cpio.gz -d $img \
        -c "console=$con quiet" -e DISK-TEST-DONE -t 300 < /dev/null 2>/dev/null | tr -d '\r')
    t1=$(date +%s)
    got=$(echo "$out" | grep '^MD5 ' | awk '{print $2}')
    ok=ok
    if [ $fmt = qcow2 ]; then
        qemu-img check -q $img || ok=FALHOU
        qemu-img convert -f qcow2 -O raw $img images/tmp/$arch.raw
        img=images/tmp/$arch.raw
    fi
    zero4m=b5cfa9d6c8febd618f91ac2843d50a1c
    disc=$(echo "$out" | grep '^DISCARD ' | awk '{print $2}')
    if [ $fmt = mvd ]; then
        [ "$disc" = $zero4m ] || ok=FALHOU
        "$IMG" check $img > images/tmp/chk.txt || ok=FALHOU
        grep -q "com dados: 48," images/tmp/chk.txt || { ok=FALHOU; cat images/tmp/chk.txt; }
        "$IMG" convert -O raw $img images/tmp/$arch.raw 2>/dev/null
        img=images/tmp/$arch.raw
    elif [ -n "$disc" ] && [ "$disc" != nenhum ] && [ "$disc" != $zero4m ]; then
        ok=FALHOU
    fi
    wrote=$(dd if=$img bs=512 skip=100 count=1 2>/dev/null | head -1 | tr -cd 'A-Za-z0-9_.-')
    echo "$out" | grep -q DISK-TEST-DONE || ok=FALHOU
    [ "$got" = "$want" ] || ok=FALHOU
    case "$wrote" in MULTIVM-GRAVOU-*) ;; *) ok=FALHOU ;; esac
    [ $fmt != raw ] && arch=$arch/$fmt
    printf "%-13s %-7s %3ss  leitura:%s  escrita:%s  discard:%s\n" $arch $ok $((t1 - t0)) "${got:-?}" "${wrote:-?}" "${disc:-?}"
    [ $ok = ok ] || fail=1
done
exit $fail
