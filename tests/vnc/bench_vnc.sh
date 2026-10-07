#!/bin/sh
# Mede o VNC com uma area de trabalho grafica real (ReactOS instalado): bytes e tempo
# de uma tela cheia em cada codificacao. Uso: ROS=disco-do-reactos.img ./bench_vnc.sh
cd "$(dirname "$0")"
MVM=${MVM:-../../core/build/mvm-cli}
BIOS=${BIOS:-/usr/share/seabios/bios-256k.bin}
ROS=${ROS:-/root/multivm-images/reactos/ros-disk-installed.img}
WAIT=${WAIT:-90}
mkdir -p out
rm -f out/ros.qcow2
qemu-img create -f qcow2 -b "$ROS" -F raw out/ros.qcow2 > /dev/null
"$MVM" -a i386 -m 512 --bios "$BIOS" -d out/ros.qcow2 --vnc :15 -t $((WAIT + 60)) < /dev/null > out/bench-srv.log 2>&1 &
sleep "$WAIT"
for e in raw hextile zrle; do
    printf '%-8s ' $e
    python3 vnc_client.py 5915 $e --ppm out/bench-$e.ppm | grep atualizacao
done
wait
rm -f out/ros.qcow2
