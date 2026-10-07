#!/usr/bin/env python3
"""Neutraliza DT_RPATH/DT_RUNPATH de um ELF64 (troca a tag por DT_DEBUG), sem mudar o layout."""
import struct, sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
phoff = struct.unpack_from('<Q', b, 0x20)[0]
phentsize, phnum = struct.unpack_from('<HH', b, 0x36)
for i in range(phnum):
    off = phoff + i * phentsize
    if struct.unpack_from('<I', b, off)[0] == 2:  # PT_DYNAMIC
        doff = struct.unpack_from('<Q', b, off + 8)[0]
        dsz = struct.unpack_from('<Q', b, off + 32)[0]
        for j in range(0, dsz, 16):
            if struct.unpack_from('<q', b, doff + j)[0] in (15, 29):
                struct.pack_into('<q', b, doff + j, 21)
open(p, 'wb').write(b)
