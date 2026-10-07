#!/usr/bin/env python3
"""
Cliente RFB minimo para testar o servidor VNC do MultiVM.
  vnc_client.py PORTA ENCODING [SENHA] [--ppm SAIDA] [--keys TEXTO] [--libdes LIB]
ENCODING: raw | hextile | zrle. Pede um quadro completo, decodifica e grava um PPM.
Com --keys, digita TEXTO (keysyms; \\n = Return) depois do quadro.
"""
import ctypes, socket, struct, sys, zlib

args = sys.argv[1:]
port, enc = int(args[0]), args[1]
password = args[2] if len(args) > 2 and not args[2].startswith('--') else None
opt = {args[i]: args[i + 1] for i in range(len(args) - 1) if args[i].startswith('--')}

s = socket.create_connection(('127.0.0.1', port), 10)
s.settimeout(20)
buf = b''
def recv(n):
    global buf
    while len(buf) < n:
        d = s.recv(65536)
        if not d:
            raise SystemExit('conexao fechada')
        buf += d
    r, buf = buf[:n], buf[n:]
    return r

ver = recv(12)
assert ver == b'RFB 003.008\n', ver
s.sendall(b'RFB 003.008\n')
n = recv(1)[0]
types = recv(n)
if 2 in types:
    if not password:
        raise SystemExit('servidor pede senha')
    s.sendall(b'\x02')
    chal = recv(16)
    des = ctypes.CDLL(opt['--libdes'])
    key = bytes(int('{:08b}'.format(c)[::-1], 2) for c in password.encode()[:8].ljust(8, b'\0'))
    out = ctypes.create_string_buffer(16)
    for i in (0, 8):
        o8 = ctypes.create_string_buffer(8)
        des.des_encrypt_block(key, chal[i:i + 8], o8)
        out[i:i + 8] = o8.raw
    s.sendall(out.raw)
else:
    s.sendall(b'\x01')
res = struct.unpack('>I', recv(4))[0]
if res != 0:
    ln = struct.unpack('>I', recv(4))[0]
    raise SystemExit('autenticacao recusada: ' + recv(ln).decode())
s.sendall(b'\x01')  # ClientInit (compartilhado)
w, h = struct.unpack('>HH', recv(4))
pf = recv(16)
name = recv(struct.unpack('>I', recv(4))[0]).decode()
print('servidor "%s": %dx%d' % (name, w, h))

codes = {'raw': 0, 'hextile': 5, 'zrle': 16, 'tight': 7}
encs = [codes[enc], -223]
if '--jpeg' in opt:  # nivel de qualidade JPEG (0-9) para o Tight
    encs.append(-32 + int(opt['--jpeg']))
s.sendall(struct.pack('>BBH', 2, 0, len(encs)) + b''.join(struct.pack('>i', e) for e in encs))
fb = bytearray(w * h * 4)
z = zlib.decompressobj()
tz = [zlib.decompressobj() for _ in range(4)]

def compact_len():
    b = recv(1)[0]; n = b & 0x7f
    if b & 0x80:
        b = recv(1)[0]; n |= (b & 0x7f) << 7
        if b & 0x80:
            n |= recv(1)[0] << 14
    return n

def tight(x0, y0, rw, rh):
    ctl = recv(1)[0]
    for i in range(4):
        if ctl & (1 << i):
            tz[i] = zlib.decompressobj()
    comp = ctl >> 4
    if comp == 8:  # preenchimento (TPIXEL R,G,B)
        r, g, b = recv(3)
        for j in range(rh):
            for i in range(rw):
                put(x0 + i, y0 + j, (b, g, r))
        return
    if comp == 9:  # JPEG
        import io
        from PIL import Image
        img = Image.open(io.BytesIO(recv(compact_len()))).convert('RGB')
        assert img.size == (rw, rh), img.size
        px = img.load()
        for j in range(rh):
            for i in range(rw):
                r, g, b = px[i, j]
                put(x0 + i, y0 + j, (b, g, r))
        return
    if comp > 9:
        raise SystemExit('tight: compressao %d' % comp)
    stream, filt = comp & 3, 0
    if comp & 4:
        filt = recv(1)[0]
    pal = None
    if filt == 1:
        n = recv(1)[0] + 1
        pal = [recv(3) for _ in range(n)]
        dlen = (rw + 7) // 8 * rh if n == 2 else rw * rh
    elif filt == 0:
        dlen = rw * rh * 3
    else:
        raise SystemExit('tight: filtro %d' % filt)
    data = recv(dlen) if dlen < 12 else tz[stream].decompress(recv(compact_len()))
    for j in range(rh):
        for i in range(rw):
            if pal is None:
                k = (j * rw + i) * 3
                r, g, b = data[k], data[k + 1], data[k + 2]
            elif len(pal) == 2:
                bit = (data[j * ((rw + 7) // 8) + i // 8] >> (7 - i % 8)) & 1
                r, g, b = pal[bit]
            else:
                r, g, b = pal[data[j * rw + i]]
            put(x0 + i, y0 + j, (b, g, r))

def pix(b):  # formato padrao: 32 bpp, little endian, R<<16 G<<8 B
    return b[0], b[1], b[2]

def put(x, y, bgr):
    i = (y * w + x) * 4
    fb[i:i + 3] = bytes(bgr)

def zrle(x0, y0, rw, rh, data):
    p = 0
    def cpix():
        nonlocal p
        v = data[p:p + 3]; p += 3
        return v
    for ty in range(y0, y0 + rh, 64):
        for tx in range(x0, x0 + rw, 64):
            tw, th = min(64, x0 + rw - tx), min(64, y0 + rh - ty)
            sub = data[p]; p += 1
            if sub == 0:
                for j in range(th):
                    for i in range(tw):
                        put(tx + i, ty + j, cpix())
            elif sub == 1:
                c = cpix()
                for j in range(th):
                    for i in range(tw):
                        put(tx + i, ty + j, c)
            elif 2 <= sub <= 16:
                pal = [cpix() for _ in range(sub)]
                bits = 1 if sub == 2 else 2 if sub <= 4 else 4
                for j in range(th):
                    acc, nb = 0, 0
                    for i in range(tw):
                        if nb == 0:
                            acc = data[p]; p += 1; nb = 8
                        nb -= bits
                        put(tx + i, ty + j, pal[(acc >> nb) & ((1 << bits) - 1)])
            elif sub == 128 or sub >= 130:
                pal = [cpix() for _ in range(sub - 128)] if sub >= 130 else None
                k = 0
                while k < tw * th:
                    if pal is None:
                        c = cpix(); ln = 1
                        while True:
                            b = data[p]; p += 1; ln += b
                            if b != 255: break
                    else:
                        b = data[p]; p += 1
                        c = pal[b & 127]; ln = 1
                        if b & 128:
                            while True:
                                b2 = data[p]; p += 1; ln += b2
                                if b2 != 255: break
                    for _ in range(ln):
                        put(tx + k % tw, ty + k // tw, c); k += 1
            else:
                raise SystemExit('subcodificacao ZRLE %d' % sub)

def read_update():
    global w, h, fb
    t = recv(1)[0]
    assert t == 0, t
    recv(1)
    nrect = struct.unpack('>H', recv(2))[0]
    for _ in range(nrect):
        x, y, rw, rh, e = struct.unpack('>HHHHi', recv(12))
        if e == -223:
            w, h = rw, rh
            fb = bytearray(w * h * 4)
            print('DesktopSize %dx%d' % (w, h))
        elif e == 0:
            d = recv(rw * rh * 4)
            for j in range(rh):
                for i in range(rw):
                    k = (j * rw + i) * 4
                    put(x + i, y + j, d[k:k + 3])
        elif e == 5:
            for ty in range(y, y + rh, 16):
                for tx in range(x, x + rw, 16):
                    tw, th = min(16, x + rw - tx), min(16, y + rh - ty)
                    sub = recv(1)[0]
                    if sub & 1:
                        d = recv(tw * th * 4)
                        for j in range(th):
                            for i in range(tw):
                                k = (j * tw + i) * 4
                                put(tx + i, ty + j, d[k:k + 3])
                    elif sub == 2:
                        c = recv(4)[:3]
                        for j in range(th):
                            for i in range(tw):
                                put(tx + i, ty + j, c)
                    else:
                        raise SystemExit('hextile %d nao suportado' % sub)
        elif e == 7:
            tight(x, y, rw, rh)
        elif e == 16:
            ln = struct.unpack('>I', recv(4))[0]
            zrle(x, y, rw, rh, z.decompress(recv(ln)))
        else:
            raise SystemExit('codificacao %d' % e)
    return nrect

import time
nread = [0]
_orig_recv = recv
def recv(n):
    nread[0] += n
    return _orig_recv(n)
t0 = time.time()
s.sendall(struct.pack('>BBHHHH', 3, 0, 0, 0, w, h))
n = read_update()
dt = time.time() - t0
print('atualizacao: %d retangulos, %d bytes, %.0f ms' % (n, nread[0], dt * 1000))
if '--ppm' in opt:
    with open(opt['--ppm'], 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(bytes(b for i in range(0, len(fb), 4) for b in (fb[i + 2], fb[i + 1], fb[i])))
if '--keys' in opt:
    shifted = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ~!@#$%^&*()_+{}|:"<>?')
    for ch in opt['--keys'].replace('\\n', '\n'):
        ks = 0xff0d if ch == '\n' else ord(ch)
        key = struct.pack('>BBxxI', 4, 1, ks) + struct.pack('>BBxxI', 4, 0, ks)
        if ch in shifted:  # como um cliente real: Shift em volta
            key = struct.pack('>BBxxI', 4, 1, 0xffe1) + key + struct.pack('>BBxxI', 4, 0, 0xffe1)
        s.sendall(key)
    print('teclas enviadas')
s.close()
