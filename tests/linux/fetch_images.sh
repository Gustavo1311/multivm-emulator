#!/bin/sh
# Baixa kernels Linux do Alpine (netboot) e busybox estatico para as 4 arquiteturas e
# monta initramfs de teste. Saida: tests/linux/images/<arch>/
set -e
cd "$(dirname "$0")"
TOOLS=../../tools
MIRROR=${MIRROR:-https://dl-cdn.alpinelinux.org/alpine/latest-stable}
mkdir -p images
for spec in aarch64:virt x86_64:virt armv7:lts x86:lts; do
    a=${spec%%:*}; flavor=${spec##*:}
    d=images/$a; mkdir -p $d
    [ -f $d/vmlinuz ] || curl -fL -o $d/vmlinuz $MIRROR/releases/$a/netboot/vmlinuz-$flavor
    [ -f $d/initramfs-alpine ] || curl -fL -o $d/initramfs-alpine $MIRROR/releases/$a/netboot/initramfs-$flavor
    if [ ! -f $d/busybox.static ]; then
        f=$(curl -s $MIRROR/main/$a/ | grep -o 'busybox-static-[0-9][^"]*\.apk' | head -1)
        curl -fL -o $d/bbs.apk $MIRROR/main/$a/$f
        mkdir -p $d/bbs && tar -xzf $d/bbs.apk -C $d/bbs 2>/dev/null || true
        cp $d/bbs/bin/busybox.static $d/busybox.static
        rm -rf $d/bbs $d/bbs.apk
    fi
    # modulos virtio do initramfs do Alpine (o disco e virtio-blk)
    python3 $TOOLS/extract_modules.py $d/initramfs-alpine $d virtio_ring virtio virtio_mmio \
        virtio_pci_legacy_dev virtio_pci_modern_dev virtio_pci virtio_blk
    python3 $TOOLS/mkinitramfs.py $d/busybox.static $d/shell.cpio.gz > /dev/null
    extra="disktest.sh:init.d"
    for m in $d/*.ko; do [ -f "$m" ] && extra="$extra $m:$(basename $m)"; done
    python3 $TOOLS/mkinitramfs.py $d/busybox.static $d/disktest.cpio.gz $extra > /dev/null
    echo "$a: pronto"
done
