#!/bin/sh
# Som com o Alpine standard (ISO com kernel LTS): para cada placa (ac97, hda) toca
# um seno de 1 kHz (speaker-test) gravado pelo --audio-wav e grava o microfone
# (--audio-in com um tom de 440 Hz) com arecord; a gravacao volta pelo NAT (nc).
# Os pacotes do alsa-utils ficam em apks/ (servidos ao convidado por HTTP); se faltarem,
# sao baixados do Alpine (lista em apks.txt, versoes da ISO 3.24).
# Uso: ISO=alpine-standard-x86_64.iso ./run_audio_tests.sh [ac97 hda]
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
BIOS=${BIOS:-/usr/share/seabios/bios-256k.bin}
ISO=${ISO:-/root/multivm-images/alpine/alpine-standard-3.24.2-x86_64.iso}
BOOT_WAIT=${BOOT_WAIT:-150}
MODELS=${*:-ac97 hda}
mkdir -p out apks
MIRROR=${MIRROR:-https://dl-cdn.alpinelinux.org/alpine/v3.24/main/x86_64}
while read -r pkg; do
    [ -f apks/$pkg ] || curl -fsSL -o apks/$pkg "$MIRROR/$pkg" || { echo "falha ao baixar $pkg"; exit 1; }
done < apks.txt
python3 - <<'PY'
import math, struct, wave
w = wave.open('out/tone440.wav', 'wb')
w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b''.join(struct.pack('<hh', int(12000 * math.sin(2 * math.pi * 440 * i / 48000)),
                                   int(12000 * math.sin(2 * math.pi * 440 * i / 48000))) for i in range(48000)))
PY
APK_PORT=18081
REC_PORT=18091
python3 -m http.server $APK_PORT --bind 127.0.0.1 --directory apks > /dev/null 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null' EXIT
fail=0
for model in $MODELS; do
    case $model in ac97) mod=snd-intel8x0 ;; hda) mod=snd-hda-intel ;; esac
    rm -f out/$model-play.wav out/$model-rec.wav out/$model.log
    python3 -c "
import socket
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $REC_PORT)); s.listen(1); s.settimeout(600)
c,_=s.accept(); f=open('out/$model-rec.wav','wb')
while True:
    d=c.recv(65536)
    if not d: break
    f.write(d)
" &
    REC_PID=$!
    (
        # espera o login (com folga ate BOOT_WAIT*3 s: o boot fica mais lento com a maquina ocupada)
        t=0; until grep -q "login:" out/$model.log 2>/dev/null || [ $t -ge $((BOOT_WAIT * 3)) ]; do sleep 2; t=$((t + 2)); done
        sleep 2; printf 'root\n'; sleep 6
        printf 'ip link set eth0 up; udhcpc -i eth0 -n -q >/dev/null 2>&1; mkdir /tmp/a; cd /tmp/a; '
        printf 'for p in $(wget -q -O - http://10.0.2.2:%s/ | grep -o "[a-z0-9_.+-]*\\.apk" | sort -u); do wget -q http://10.0.2.2:%s/$p; done; ' $APK_PORT $APK_PORT
        printf 'apk add -q --allow-untrusted --force-non-repository *.apk; cd; modprobe %s; sleep 2; ' $mod
        printf 'amixer scontrols | cut -d"'"'"'" -f2 | while read c; do amixer -q sset "$c" 100%% unmute cap 2>/dev/null; done; cat /proc/asound/cards; echo SETUP-""DONE\n'
        sleep 120
        printf 'speaker-test -D hw:0 -t sine -f 1000 -c 2 -r 48000 -l 1 -b 200000 -p 50000 > /dev/null 2>&1 && echo PLAY-""OK\n'
        sleep 60
        printf 'arecord -D hw:0 -f S16_LE -r 48000 -c 2 -d 3 /tmp/rec.wav 2>/dev/null && echo REC-""OK; nc 10.0.2.2 %s < /tmp/rec.wav; echo AUDIO-TEST-""DONE\n' $REC_PORT
        sleep 120
    ) | "$MVM" -a x86_64 -m 768 --bios "$BIOS" --cdrom "$ISO" --net virtio --audio $model \
        --audio-wav out/$model-play.wav --audio-in out/tone440.wav -t 1200 -e AUDIO-TEST-DONE > out/$model.log 2>&1
    sleep 2
    kill $REC_PID 2>/dev/null
    clean=$(tr -d '\r' < out/$model.log)
    ok=ok
    echo "$clean" | grep -q "^PLAY-OK" || { ok=FALHOU; echo "  $model: speaker-test falhou"; }
    echo "$clean" | grep -q "^REC-OK" || { ok=FALHOU; echo "  $model: arecord falhou"; }
    python3 wavcheck.py out/$model-play.wav 1000 > out/$model-play.txt || ok=FALHOU; sed "s/^/  $model saida: /" out/$model-play.txt
    python3 wavcheck.py out/$model-rec.wav 440 > out/$model-rec.txt || ok=FALHOU; sed "s/^/  $model microfone: /" out/$model-rec.txt
    echo "$model: $ok"
    [ $ok = ok ] || fail=1
done
exit $fail
