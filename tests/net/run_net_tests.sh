#!/bin/sh
# Rede NAT em modo usuario com o Alpine (ISO), uma vez por placa: DHCP, ping no
# gateway, HTTP para o host (10.0.2.2), DNS (10.0.2.3) e redirecionamento de
# porta do host para o convidado. Comandos pela serial (como no teste de disquete).
# A ISO "standard" (kernel LTS) tem os drivers das tres placas; a "virt" nao tem o 8139too.
# Uso: ISO=alpine-standard-x86_64.iso ./run_net_tests.sh [rtl8139 e1000 virtio]
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
BIOS=${BIOS:-/usr/share/seabios/bios-256k.bin}
ISO=${ISO:-/root/multivm-images/alpine/alpine-standard-3.24.2-x86_64.iso}
BOOT_WAIT=${BOOT_WAIT:-150}
MODELS=${*:-rtl8139 e1000 virtio}
mkdir -p out/www
echo "MULTIVM-HTTP-OK" > out/www/hello.txt
HTTP_PORT=18080
FWD_PORT=15555
python3 -m http.server $HTTP_PORT --bind 127.0.0.1 --directory out/www > out/http.log 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null' EXIT
fail=0
for model in $MODELS; do
    log=out/$model.log
    rm -f "$log"
    (
        # espera o login (com folga ate BOOT_WAIT*3 s: o boot fica mais lento com a maquina ocupada)
        t=0; until grep -q "login:" $log 2>/dev/null || [ $t -ge $((BOOT_WAIT * 3)) ]; do sleep 2; t=$((t + 2)); done
        sleep 2; printf 'root\n'; sleep 6
        printf 'ip link set eth0 up; udhcpc -i eth0 -n -q -t 10 2>&1 | tail -2; ip -4 addr show eth0 | grep inet\n'
        sleep 8
        printf 'ping -c 2 -W 2 10.0.2.2 > /dev/null && echo PING-""OK\n'; sleep 6
        printf 'wget -q -O - http://10.0.2.2:%s/hello.txt\n' $HTTP_PORT; sleep 5
        printf 'nslookup dl-cdn.alpinelinux.org 10.0.2.3 2>&1 | grep -q Address && echo DNS-""OK\n'; sleep 8
        printf '(echo MULTIVM-FWD-OK | nc -l -p 7777 &) ; sleep 1; echo LISTEN-""READY\n'; sleep 4
        python3 -c "
import socket,time
for i in range(10):
    try:
        s=socket.create_connection(('127.0.0.1',$FWD_PORT),3); s.settimeout(5)
        d=s.recv(100); open('out/$model.fwd','wb').write(d); break
    except Exception as e: time.sleep(1)
" &
        sleep 8
        printf 'head -c 65536 /dev/urandom | wc -c; wget -q -O - http://10.0.2.2:%s/big.bin | md5sum\n' $HTTP_PORT
        sleep 20
        printf 'echo NET-TEST-""DONE\n'
        sleep 30
    ) | "$MVM" -a x86_64 -m 768 --bios "$BIOS" --cdrom "$ISO" --net $model --hostfwd tcp:$FWD_PORT-:7777 \
        -t 900 -e NET-TEST-DONE > $log 2>&1
    clean=$(tr -d '\r' < $log)
    ok=ok
    echo "$clean" | grep -q "inet 10.0.2.15" || { ok=FALHOU; echo "  $model: sem DHCP"; }
    echo "$clean" | grep -q "^PING-OK" || { ok=FALHOU; echo "  $model: ping falhou"; }
    echo "$clean" | grep -q "^MULTIVM-HTTP-OK" || { ok=FALHOU; echo "  $model: HTTP falhou"; }
    echo "$clean" | grep -q "^DNS-OK" || echo "  $model: DNS sem resposta (o host tem internet?)"
    grep -q MULTIVM-FWD-OK out/$model.fwd 2>/dev/null || { ok=FALHOU; echo "  $model: redirecionamento falhou"; }
    want=$(md5sum < out/www/big.bin | cut -d' ' -f1)
    echo "$clean" | grep -q "^$want" || { ok=FALHOU; echo "  $model: download de 2 MiB falhou"; }
    echo "$model: $ok"
    [ $ok = ok ] || fail=1
done
exit $fail
