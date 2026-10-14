#!/bin/sh
# Baixa as ISOs usadas por run_bsd_tests.sh (oficiais, conferidas pelo hash publicado).
# Destino: $BSD_DIR (padrao images/). Uns 2,3 GB no total.
set -e
D=${BSD_DIR:-$(dirname "$0")/images}
mkdir -p "$D"; cd "$D"
get() { [ -s "$1" ] || curl -fL --retry 5 --retry-all-errors -C - -o "$1" "$2"; }
get openbsd79-amd64-install.iso https://cdn.openbsd.org/pub/OpenBSD/7.9/amd64/install79.iso
get openbsd79-i386-cd.iso https://cdn.openbsd.org/pub/OpenBSD/7.9/i386/cd79.iso
get NetBSD-11.0-amd64.iso https://cdn.netbsd.org/pub/NetBSD/iso/11.0/NetBSD-11.0-amd64.iso
if [ ! -s FreeBSD-15.1-RELEASE-amd64-disc1.iso ]; then
    get FreeBSD-15.1-RELEASE-amd64-disc1.iso.xz \
        https://download.freebsd.org/releases/amd64/amd64/ISO-IMAGES/15.1/FreeBSD-15.1-RELEASE-amd64-disc1.iso.xz
    xz -d FreeBSD-15.1-RELEASE-amd64-disc1.iso.xz
fi
curl -fsSL -o SHA256.openbsd-amd64 https://cdn.openbsd.org/pub/OpenBSD/7.9/amd64/SHA256
curl -fsSL -o SHA256.openbsd-i386 https://cdn.openbsd.org/pub/OpenBSD/7.9/i386/SHA256
curl -fsSL -o SHA512.netbsd https://cdn.netbsd.org/pub/NetBSD/iso/11.0/SHA512
curl -fsSL -o CHECKSUM.freebsd https://download.freebsd.org/releases/amd64/amd64/ISO-IMAGES/15.1/CHECKSUM.SHA256-FreeBSD-15.1-RELEASE-amd64
chk() { [ "$($1 "$2" | cut -d' ' -f1)" = "$(grep "($3)" "$4" | sed 's/.* = //')" ] && echo "ok   $2" || { echo "FALHA hash de $2"; exit 1; }; }
chk sha256sum openbsd79-amd64-install.iso install79.iso SHA256.openbsd-amd64
chk sha256sum openbsd79-i386-cd.iso cd79.iso SHA256.openbsd-i386
chk sha512sum NetBSD-11.0-amd64.iso NetBSD-11.0-amd64.iso SHA512.netbsd
chk sha256sum FreeBSD-15.1-RELEASE-amd64-disc1.iso FreeBSD-15.1-RELEASE-amd64-disc1.iso CHECKSUM.freebsd
