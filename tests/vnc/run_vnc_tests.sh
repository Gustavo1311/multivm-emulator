#!/bin/sh
# Servidor VNC: com a tela parada do SeaBIOS (sem disco), cada codificacao (raw,
# hextile, zrle) tem de reproduzir o framebuffer (fora o cursor de texto, que
# pisca); senha errada tem de ser recusada e a certa aceita.
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
BIOS=${BIOS:-/usr/share/seabios/bios-256k.bin}
mkdir -p out
cc -shared -fPIC -O2 -I../../core/include -o out/libdes.so ../../core/src/des.c || exit 1
fail=0
"$MVM" -a i386 -m 32 --bios "$BIOS" --vnc :13 --vnc-password teste123 -t 30 --fb-dump out/fb.ppm < /dev/null > out/srv.log 2>&1 &
sleep 15
python3 vnc_client.py 5913 zrle errada --libdes out/libdes.so > out/bad.txt 2>&1
grep -q "senha incorreta" out/bad.txt && echo "ok   senha errada recusada" || { echo "FALHA senha errada aceita"; fail=1; }
for e in raw hextile zrle tight; do
    python3 vnc_client.py 5913 $e teste123 --libdes out/libdes.so --ppm out/$e.ppm > out/$e.txt 2>&1 || { echo "FALHA $e (veja out/$e.txt)"; fail=1; }
done
# Tight com JPEG (perde qualidade): so com o Pillow para decodificar
if python3 -c "import PIL" 2>/dev/null; then
    python3 vnc_client.py 5913 tight teste123 --libdes out/libdes.so --jpeg 8 --ppm out/jpeg.ppm > out/jpeg.txt 2>&1 \
        && echo "ok   tight+jpeg decodificado" || { echo "FALHA tight+jpeg (veja out/jpeg.txt)"; fail=1; }
fi
wait
for e in raw hextile zrle tight; do
    python3 - "$e" <<'PY' || fail=1
import sys
e = sys.argv[1]
a = open('out/fb.ppm', 'rb').read().split(b'\n', 3)
b = open('out/%s.ppm' % e, 'rb').read().split(b'\n', 3)
W = int(a[1].split()[0])
diff = [i // 3 for i in range(0, len(a[3]), 3) if a[3][i:i + 3] != b[3][i:i + 3]]
ok = a[1] == b[1] and len(diff) <= 32  # o cursor de texto (8x2) pode piscar entre as capturas
print('%s %-8s %s, %d pixels diferentes' % ('ok  ' if ok else 'FALHA', e, a[1].decode(), len(diff)))
sys.exit(0 if ok else 1)
PY
done
exit $fail
