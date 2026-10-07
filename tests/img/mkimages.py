#!/usr/bin/env python3
"""Gera imagens de teste que o qemu-img nao cria.

  mkimages.py vhddiff PAI.vhd FILHO.vhd PAI.raw ESPERADO.raw
      VHD diferencial (pai pelo localizador W2ru relativo) com alguns setores proprios;
      ESPERADO.raw = conteudo do pai com esses setores trocados.
  mkimages.py vhdxlog IMG.vhdx ATUAL.raw ESPERADO.raw
      acrescenta ao VHDX uma entrada de log pendente (LogGuid no cabecalho) que troca 4 KiB
      de um bloco alocado; ESPERADO.raw = ATUAL.raw com a troca aplicada.
"""
import os
import struct
import sys
import uuid


def crc32c(data):
    tab = crc32c.tab
    if not tab:
        for i in range(256):
            c = i
            for _ in range(8):
                c = (c >> 1) ^ (0x82F63B78 if c & 1 else 0)
            tab.append(c)
    crc = 0xFFFFFFFF
    for b in data:
        crc = tab[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


crc32c.tab = []


def vhd_checksum(b, skip):
    return (~sum(x for i, x in enumerate(b) if not skip <= i < skip + 4)) & 0xFFFFFFFF


def vhddiff(parent, child, parent_raw, expected):
    p = open(parent, 'rb').read()
    pf = bytearray(p[-512:])
    assert pf[:8] == b'conectix'
    dh_off = struct.unpack('>Q', pf[16:24])[0]
    pdh = p[dh_off:dh_off + 1024]
    max_entries, bsize = struct.unpack('>II', pdh[28:36])
    bm = ((bsize // 512 // 8) + 511) & ~511
    bat_off = 1536
    bat_len = (max_entries * 4 + 511) & ~511
    loc_off = bat_off + bat_len
    name = os.path.basename(parent)
    loc = name.encode('utf-16-le')
    loc_sz = (len(loc) + 511) & ~511
    data_start = loc_off + loc_sz

    foot = bytearray(pf)
    struct.pack_into('>I', foot, 60, 4)               # diferencial
    struct.pack_into('>Q', foot, 16, 512)             # cabecalho dinamico
    foot[68:84] = uuid.uuid4().bytes
    struct.pack_into('>I', foot, 64, 0)
    struct.pack_into('>I', foot, 64, vhd_checksum(foot, 64))

    dh = bytearray(1024)
    dh[0:8] = b'cxsparse'
    struct.pack_into('>QQIII', dh, 8, 0xFFFFFFFFFFFFFFFF, bat_off, 0x00010000, max_entries, bsize)
    dh[40:56] = pf[68:84]                             # uuid do pai
    pn = name.encode('utf-16-be')
    dh[64:64 + len(pn)] = pn
    struct.pack_into('>IIIIQ', dh, 576, 0x57327275, loc_sz, len(loc), 0, loc_off)  # W2ru
    struct.pack_into('>I', dh, 36, vhd_checksum(dh, 36))

    exp = bytearray(open(parent_raw, 'rb').read())
    vsize = struct.unpack('>Q', pf[48:56])[0]
    bat = [0xFFFFFFFF] * max_entries
    blocks = []
    rnd = os.urandom
    # bloco 1: so alguns trechos de setores sao do filho; bloco 3: inteiro
    plan = [(1, [(0, 100), (3000, 4000)]), (3, [(0, bsize // 512)])]
    at = data_start
    for bi, ranges in plan:
        if (bi + 1) * bsize > vsize:
            continue
        bitmap = bytearray(bm)
        data = bytearray(bsize)
        for a, b in ranges:
            for s in range(a, b):
                bitmap[s // 8] |= 0x80 >> (s % 8)
            chunk = rnd((b - a) * 512)
            data[a * 512:b * 512] = chunk
            g = bi * bsize + a * 512
            exp[g:g + len(chunk)] = chunk
        bat[bi] = at // 512
        blocks.append((at, bytes(bitmap) + bytes(data)))
        at += bm + bsize
    with open(child, 'wb') as f:
        f.write(foot)
        f.write(dh)
        f.write(b''.join(struct.pack('>I', x) for x in bat).ljust(bat_len, b'\xff'))
        f.write(loc.ljust(loc_sz, b'\0'))
        for off, blob in blocks:
            f.seek(off)
            f.write(blob)
        f.seek(at)
        f.write(foot)
    open(expected, 'wb').write(exp)


def guid_bytes(s):
    return uuid.UUID(s).bytes_le


def vhdxlog(img, current_raw, expected):
    f = open(img, 'r+b')
    hdrs = []
    for slot in (0, 1):
        f.seek((slot + 1) * 65536)
        h = bytearray(f.read(4096))
        hdrs.append((struct.unpack('<Q', h[8:16])[0], slot, h))
    seq, slot, h = max(hdrs)
    log_len, log_off = struct.unpack('<IQ', h[68:80])
    # BAT pela tabela de regioes
    f.seek(192 * 1024)
    rt = f.read(65536)
    n = struct.unpack('<I', rt[8:12])[0]
    bat_off = None
    for i in range(n):
        e = rt[16 + 32 * i:48 + 32 * i]
        if e[:16] == guid_bytes('2DC27766-F623-4200-9D64-115E9BFD4A08'):
            bat_off = struct.unpack('<Q', e[16:24])[0]
    # metadados: tamanho do bloco
    f.seek(bat_off)
    bat = struct.unpack('<64Q', f.read(512))
    target = None
    for i, e in enumerate(bat):
        if e & 7 == 6:
            target = (i, (e >> 20) << 20)
            break
    assert target, 'nenhum bloco alocado'
    bi, boff = target
    chunk = 4096 * 5
    fo = boff + chunk
    data = os.urandom(4096)
    S = 12345
    lguid = uuid.uuid4().bytes_le
    fsize = os.fstat(f.fileno()).st_size
    entry = bytearray(8192)
    entry[0:4] = b'loge'
    struct.pack_into('<IIQII', entry, 8, 8192, 0, S, 1, 0)
    entry[32:48] = lguid
    struct.pack_into('<QQ', entry, 48, fsize, fsize)
    d = 64
    entry[d:d + 4] = b'desc'
    entry[d + 4:d + 8] = data[4092:4096]
    entry[d + 8:d + 16] = data[0:8]
    struct.pack_into('<QQ', entry, d + 16, fo, S)
    s = 4096
    entry[s:s + 4] = b'data'
    struct.pack_into('<I', entry, s + 4, S >> 32)
    entry[s + 8:s + 4092] = data[8:4092]
    struct.pack_into('<I', entry, s + 4092, S & 0xFFFFFFFF)
    struct.pack_into('<I', entry, 4, crc32c(bytes(entry)))
    f.seek(log_off)
    f.write(entry)
    h[48:64] = lguid
    struct.pack_into('<I', h, 4, 0)
    struct.pack_into('<I', h, 4, crc32c(bytes(h)))
    f.seek((slot + 1) * 65536)
    f.write(h)
    f.close()
    exp = bytearray(open(current_raw, 'rb').read())
    bsize = int(os.environ['VHDX_BLOCK'])
    g = bi * bsize + chunk  # bloco do primeiro chunk: indice da BAT = indice do bloco
    exp[g:g + 4096] = data
    open(expected, 'wb').write(exp)


if __name__ == '__main__':
    {'vhddiff': vhddiff, 'vhdxlog': vhdxlog}[sys.argv[1]](*sys.argv[2:])
