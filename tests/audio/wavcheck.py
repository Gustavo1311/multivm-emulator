#!/usr/bin/env python3
"""wavcheck.py ARQ.wav FREQ: confere se o WAV (16 bits) tem um tom em FREQ Hz (DFT de Goertzel)."""
import math, struct, sys, wave

def tone_power(samples, rate, f):
    w = 2 * math.pi * f / rate
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + c * s1 - s2
        s2, s1 = s1, s0
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / max(1, len(samples)) ** 2

path, want = sys.argv[1], float(sys.argv[2])
w = wave.open(path)
rate, nch, n = w.getframerate(), w.getnchannels(), w.getnframes()
raw = w.readframes(n) if n else open(path, 'rb').read()[44:]  # cabecalho ainda nao finalizado
n = len(raw) // (2 * nch)
data = struct.unpack('<%dh' % (n * nch), raw[:n * nch * 2])
ok = True
for ch in range(nch):
    x = data[ch::nch]
    loud = [i for i, v in enumerate(x) if abs(v) > 1000]
    if not loud:
        print('canal %d: silencio' % ch)
        continue
    seg = x[loud[0]:loud[0] + rate // 2]  # meio segundo a partir do inicio do som
    p_want = tone_power(seg, rate, want)
    others = [tone_power(seg, rate, f) for f in (want * 0.5, want * 1.5, want * 2.3, want * 3.7)]
    peak = max(abs(v) for v in seg)
    good = p_want > 20 * max(others)
    print('canal %d: %.2f s com som, pico %d, tom de %.0f Hz %s' % (ch, len(loud) / rate, peak, want, 'OK' if good else 'AUSENTE'))
    ok = ok and good
print('duracao %.2f s a %d Hz' % (n / rate, rate))
sys.exit(0 if ok else 1)
