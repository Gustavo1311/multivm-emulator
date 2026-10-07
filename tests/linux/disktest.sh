#!/bin/sh
# executado dentro do convidado (/init.d): carrega virtio-blk, le e grava no disco, desliga
for m in virtio_ring virtio virtio_mmio virtio_pci_legacy_dev virtio_pci_modern_dev virtio_pci virtio_blk; do
    [ -f /$m.ko ] && insmod /$m.ko 2>/dev/null
done
sleep 1
cat /proc/partitions
echo "MD5 $(dd if=/dev/vda bs=1k count=16 2>/dev/null | md5sum)"
echo "MULTIVM-GRAVOU-$(uname -m)" | dd of=/dev/vda bs=512 seek=100 conv=notrunc 2>/dev/null
sync
# TRIM: so se o disco anuncia discard (formato que libera espaco); a faixa tem de voltar zerada
if [ "$(cat /sys/block/vda/queue/discard_max_bytes 2>/dev/null)" != "0" ] && [ -e /sys/block/vda/queue/discard_max_bytes ]; then
    blkdiscard -o 1048576 -l 4194304 /dev/vda && sync
    echo "DISCARD $(dd if=/dev/vda bs=1M skip=1 count=4 2>/dev/null | md5sum)"
else
    echo "DISCARD nenhum"
fi
echo DISK-TEST-DONE
poweroff -f
