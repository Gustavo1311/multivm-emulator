#!/usr/bin/env python3
"""Cria um initramfs (cpio newc + gzip) minimo com um busybox estatico.
Uso: mkinitramfs.py <busybox> <saida.cpio.gz> [arquivo_extra:destino ...]"""
import gzip, os, stat, sys

entries = []
ino = [1000]

def add(name, mode, data=b"", rdev=(0, 0)):
    ino[0] += 1
    entries.append((name, mode, data, rdev, ino[0]))

def hdr(name, mode, size, rdev, inode):
    nb = name.encode() + b"\0"
    h = "070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x" % (
        inode, mode, 0, 0, 1, 0, size, 0, 0, rdev[0], rdev[1], len(nb), 0)
    out = h.encode() + nb
    out += b"\0" * ((4 - len(out) % 4) % 4)
    return out

for d in ["bin", "sbin", "usr", "usr/bin", "usr/sbin", "proc", "sys", "dev", "tmp", "etc", "root", "mnt"]:
    add(d, stat.S_IFDIR | 0o755)
add("dev/console", stat.S_IFCHR | 0o600, rdev=(5, 1))
add("dev/null", stat.S_IFCHR | 0o666, rdev=(1, 3))
add("dev/ttyS0", stat.S_IFCHR | 0o600, rdev=(4, 64))
add("bin/busybox", stat.S_IFREG | 0o755, open(sys.argv[1], "rb").read())
init = b"""#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null
echo "MultiVM: initramfs pronto ($(uname -m))"
[ -x /init.d ] && /init.d
exec sh
"""
add("init", stat.S_IFREG | 0o755, init)
for extra in sys.argv[3:]:
    src, dst = extra.split(":")
    add(dst.lstrip("/"), stat.S_IFREG | 0o755, open(src, "rb").read())

out = b""
for name, mode, data, rdev, inode in entries:
    out += hdr(name, mode, len(data), rdev, inode) + data
    out += b"\0" * ((4 - len(data) % 4) % 4)
out += hdr("TRAILER!!!", 0, 0, (0, 0), 0)
with gzip.open(sys.argv[2], "wb") as f:
    f.write(out)
print("ok", sys.argv[2], len(out))
