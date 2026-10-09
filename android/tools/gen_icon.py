#!/usr/bin/env python3
"""Gera o icone do app (PNGs em todas as densidades + camadas do icone adaptativo).

Python puro (sem PIL): desenha formas simples numa grade 4x maior e reduz com media
(antialias). Uso: python3 tools/gen_icon.py  (a partir de android/)."""
import os, struct, sys, zlib

RES = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'app', 'src', 'main', 'res')
K = 4  # supersampling

LIGHT = (0x4F, 0xC3, 0xF7)
LIGHT2 = (0x81, 0xD4, 0xFA)
SCREEN = (0x0B, 0x1E, 0x2E)
CHIP = (0x16, 0x2B, 0x40)
BG_TOP = (0x1E, 0x3A, 0x5F)
BG_BOT = (0x0E, 0x16, 0x21)


def rrect(x0, y0, x1, y1, r):
    def inside(x, y):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return (x0, y0, x1, y1), inside


def art(mono=False):
    """Formas do desenho em coordenadas 0..1 (monitor com um chip na tela)."""
    sh = []
    def add(shape, color, light=True):
        if not mono or light:
            sh.append((shape, (255, 255, 255) if mono else color))
    add(rrect(0.04, 0.10, 0.96, 0.74, 0.08), LIGHT)            # moldura do monitor
    add(rrect(0.10, 0.16, 0.90, 0.68, 0.04), SCREEN, False)    # tela
    add(rrect(0.42, 0.74, 0.58, 0.84, 0.0), LIGHT)             # pescoco
    add(rrect(0.26, 0.83, 0.74, 0.90, 0.035), LIGHT)           # base
    # chip com pinos no centro da tela
    cx0, cy0, cx1, cy1 = 0.36, 0.26, 0.64, 0.58
    for i in range(3):
        t = cx0 + 0.055 + i * 0.085
        add(rrect(t, 0.205, t + 0.035, cy0 + 0.01, 0.0), LIGHT2)   # pinos de cima
        add(rrect(t, cy1 - 0.01, t + 0.035, 0.635, 0.0), LIGHT2)   # pinos de baixo
        u = cy0 + 0.055 + i * 0.085
        add(rrect(0.305, u, cx0 + 0.01, u + 0.035, 0.0), LIGHT2)   # pinos da esquerda
        add(rrect(cx1 - 0.01, u, 0.695, u + 0.035, 0.0), LIGHT2)   # pinos da direita
    add(rrect(cx0, cy0, cx1, cy1, 0.035), LIGHT)               # corpo do chip
    add(rrect(cx0 + 0.04, cy0 + 0.04, cx1 - 0.04, cy1 - 0.04, 0.02), CHIP, False)
    add(rrect(0.455, 0.375, 0.545, 0.465, 0.012), LIGHT)       # nucleo
    return sh


def render(size, bg=None, art_box=(0.0, 0.0, 1.0, 1.0), mono=False):
    """bg: None (transparente), 'square' (quadrado arredondado) ou 'circle'."""
    n = size * K
    px = bytearray(n * n * 4)
    def fill(bbox, inside, color_at):
        x0, y0, x1, y1 = bbox
        for j in range(max(0, int(y0 * n)), min(n, int(y1 * n) + 1)):
            y = (j + 0.5) / n
            row = j * n * 4
            col = color_at(y)
            for i in range(max(0, int(x0 * n)), min(n, int(x1 * n) + 1)):
                x = (i + 0.5) / n
                if inside(x, y):
                    o = row + i * 4
                    px[o:o + 4] = bytes((col[0], col[1], col[2], 255))
    def grad(y):
        return tuple(int(BG_TOP[c] + (BG_BOT[c] - BG_TOP[c]) * y) for c in range(3))
    if bg == 'square':
        fill(*rrect(0.03, 0.03, 0.97, 0.97, 0.21), grad)
    elif bg == 'circle':
        fill((0.02, 0.02, 0.98, 0.98), lambda x, y: (x - 0.5) ** 2 + (y - 0.5) ** 2 <= 0.48 ** 2, grad)
    ax0, ay0, ax1, ay1 = art_box
    sx, sy = ax1 - ax0, ay1 - ay0
    for (bbox, inside), color in art(mono):
        b = (ax0 + bbox[0] * sx, ay0 + bbox[1] * sy, ax0 + bbox[2] * sx, ay0 + bbox[3] * sy)
        fill(b, lambda x, y, inside=inside: inside((x - ax0) / sx, (y - ay0) / sy), lambda y, c=color: c)
    # reduz K x K -> 1 (media com alfa pre-multiplicado)
    out = bytearray(size * size * 4)
    kk = K * K
    for j in range(size):
        for i in range(size):
            r = g = b = a = 0
            for dj in range(K):
                o = ((j * K + dj) * n + i * K) * 4
                for di in range(K):
                    pa = px[o + 3]
                    r += px[o] * pa; g += px[o + 1] * pa; b += px[o + 2] * pa; a += pa
                    o += 4
            q = (j * size + i) * 4
            if a:
                out[q] = r // a; out[q + 1] = g // a; out[q + 2] = b // a; out[q + 3] = a // kk
    return out


def write_png(path, size, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * size * 4:(y + 1) * size * 4]) for y in range(size))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


DENS = {'mdpi': 1.0, 'hdpi': 1.5, 'xhdpi': 2.0, 'xxhdpi': 3.0, 'xxxhdpi': 4.0}
LEGACY_ART = (0.16, 0.17, 0.84, 0.83)
FG_ART = (0.2, 0.21, 0.8, 0.79)  # dentro da zona segura (66 de 108 dp)

if __name__ == '__main__':
    for d, f in DENS.items():
        s = int(48 * f)
        write_png(f'{RES}/mipmap-{d}/ic_launcher.png', s, render(s, 'square', LEGACY_ART))
        write_png(f'{RES}/mipmap-{d}/ic_launcher_round.png', s, render(s, 'circle', LEGACY_ART))
        s = int(108 * f)
        write_png(f'{RES}/mipmap-{d}/ic_launcher_foreground.png', s, render(s, None, FG_ART))
        write_png(f'{RES}/mipmap-{d}/ic_launcher_monochrome.png', s, render(s, None, FG_ART, mono=True))
        print(d, file=sys.stderr)
    if '--512' in sys.argv:
        write_png(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icon-512.png'), 512,
                  render(512, 'square', LEGACY_ART))
