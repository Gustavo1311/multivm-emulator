# MultiVM

Emulador de sistema completo escrito do zero em C, com API Java para Android.
Emula quatro arquiteturas de CPU — **x86-64, i386, ARM (ARMv7-A) e ARM64 (ARMv8-A)** —
por interpretação, junto com os dispositivos necessários para rodar sistemas operacionais
convidados. A interface de usuário ainda não existe: este repositório contém o núcleo,
a ponte JNI e a biblioteca Android (`.aar`).

## Estado atual

| Arquitetura | Máquina | Linux real testado | Userland | Disco virtio | Desempenho* |
|---|---|---|---|---|---|
| ARM64 | `virt` (GICv2, PL011, PL031, virtio-mmio) | Alpine 6.18 `virt` | musl/busybox dinâmico | leitura + escrita | 35–85 MIPS |
| ARM | `virt` (idem) | Alpine 6.18 `lts` (zImage) | Thumb-2 + VFP (armhf) | leitura + escrita | 30–75 MIPS |
| x86-64 | `pc` (8259, 8254, RTC, 16550, i8042, PCI) | Alpine 6.18 `virt` (bzImage, com PTI) | musl/busybox dinâmico | leitura + escrita (virtio-pci) | JIT: ~490 MIPS |
| i386 | `pc` (idem) | Alpine 6.18 `lts` | musl/busybox dinâmico | leitura + escrita (virtio-pci) | JIT: ~245 MIPS |

\* medido em um celular ARM64 (Cortex-A55). ARM: interpretador. x86: JIT para AArch64
(benchmark de CPU `tests/guest/out/bench-*.elf`; o interpretador puro faz 15–37 MIPS). O boot
do kernel Linux até o shell leva de 7 a 12 s.

**Boot por BIOS (x86):** com o SeaBIOS a máquina `pc` inicia como um PC real. Ela dá boot
por CD-ROM (ISO) ou disco IDE, mostra a tela VGA em modo texto e gráfico (VBE) e aceita o
teclado PS/2. A ISO do Alpine 3.24 (x86-64 e i386) inicia pelo ISOLINUX até o `login:`,
tanto como CD quanto como disco rígido (ISO híbrida).

**ACPI (x86):** tabelas RSDP/XSDT/RSDT, FADT, FACS, MADT e DSDT (PCI0 com `_CRS`/`_PRT`, links
PIRQ, dispositivos ISA, `_S5`), entregues ao SeaBIOS pelo `etc/table-loader` do fw_cfg ou
instaladas em 0xE0000 no boot direto. O bloco de energia (PM1, PM timer, GPE0, SCI na IRQ 9)
permite desligar pelo sistema convidado (`poweroff`) e apertar o botão de energia pelo host.

**Sistemas da família Windows NT:** o ReactOS 0.4.15 (compatível com o NT 5.2) instala pelo
CD e roda até a área de trabalho, com instalação em modo texto, assistente gráfico em
800x600, mouse e teclado PS/2, prompt de comando e desligamento pelo ACPI. É o teste de
compatibilidade com o Windows. Com o JIT, o ReactOS instalado vai do BIOS à área de trabalho em
cerca de 22 s (eram ~15 min no interpretador).

**Windows Server 2025 (x64):** instala do começo ao fim e chega à área de trabalho (Server
Manager), com a ISO de avaliação oficial da Microsoft: instalador novo pelo CD SATA, disco AHCI,
cópia da imagem, reinicializações, configuração da senha do Administrador, logon e desligamento
pelo ACPI. No celular de teste (Cortex-A55) a instalação completa leva cerca de 15 h; o tempo vai
sobretudo para a verificação de assinaturas (RSA/SHA-256 em software) e para o DWM, que compõe a
tela por renderização em software (WARP). O mouse funciona com a aceleração de ponteiro do Windows.

**Windows XP (32 bits):** uma imagem qcow2 já instalada boota pelo disco IDE,
instala os dispositivos novos na primeira inicialização e chega à área de trabalho (cerca de
4 min no primeiro boot e 2 a 3 min nos seguintes, no celular de teste). O XP não tem driver
AHCI: o disco precisa ficar no controlador IDE (o padrão com `--bios`; no Android, tipo `DISK`,
não `SATA`), senão ele para com a tela azul 0x7B.

**Windows 10 (x64):** o instalador do 22H2 de avaliação também instala, mas esse build de 2022
está vencido: o serviço de licença da avaliação desliga a máquina cerca de uma hora após cada boot.

**Rede (NAT em modo usuário, `core/src/net/`):** sem root e sem tap, como o "user networking" do
QEMU. O convidado recebe 10.0.2.15 por DHCP; o gateway 10.0.2.2 é o próprio host e o DNS 10.0.2.3 é
repassado ao servidor DNS do host. TCP, UDP e ping viram sockets comuns do host, e portas do host podem
ser redirecionadas para o convidado (`--hostfwd tcp:2222-:22`). Placas: **RTL8139** (Windows XP, ReactOS,
Linux), **Intel e1000** (Windows 7/10/11/Server, Linux; com offload de checksum e TSO) e **virtio-net**
(Linux; também na máquina ARM). Testado com o Alpine (DHCP, ping, HTTP, DNS, download de 2 MiB,
redirecionamento, `apk` por HTTPS) e com o Windows XP (DHCP, ping, TCP).

**Som (`core/src/audio.c`):** placas **AC'97** (XP, ReactOS, Linux), **Intel HD Audio** (Windows Vista
e mais novos, Linux) e **virtio-sound** (ARM), com saída e microfone. O DMA segue o relógio virtual e o
áudio passa por anéis curtos (~20 ms) até o host: AAudio em modo de baixa latência no Android,
`--audio-wav`/`--audio-in` na linha de comando. Testado com `speaker-test`/`arecord` no Alpine.

**Servidor VNC (`core/src/vnc.c`):** RFB 3.3/3.7/3.8 com senha (autenticação VNC/DES), codificações
Tight (com JPEG, `core/src/jpeg_enc.c`), ZRLE, Hextile e Raw, redimensionamento da tela (DesktopSize),
teclado e mouse (`--vnc :1`). A compressão deflate usa a zlib do sistema carregada com `dlopen`
(`core/src/zlib_dl.c`): uma tela cheia da área de trabalho do XP cai de ~1,9 MB (Hextile) para ~170 KB
(Tight+JPEG) — `tests/vnc/bench_vnc.sh`.

**Troca de mídias com a VM ligada:** `mvm_media_list/insert/eject/add_disk` trocam o CD (ATAPI com
UNIT ATTENTION e evento de mídia), o disquete (linha de troca do 82078) e conectam/desconectam discos
SATA (hot-plug AHCI). Disco IDE não troca a quente. Com BIOS, o app sempre cria um drive de CD e o A:
(vazios se nada for escolhido); na CLI, `--cdrom none`/`--fda none`. Teste: `tests/media/run_media_tests.sh`.

**Disquete:** controlador 82078 com DMA ISA 8237, dois drives (160 KiB a 2,88 MiB), boot pelo disquete
(`--fda`, `--boot a`).

## O que está implementado

**CPUs** (`core/src/cpu/`)
- **AArch64**: conjunto inteiro completo, LSE (CAS/LDADD/SWP), CRC32, ponto flutuante escalar,
  Advanced SIMD (NEON) amplo, EL0/EL1, MMU estágio 1 (granulos 4K/64K, TBI), exceções,
  registradores de sistema, timer genérico, PSCI via HVC/SMC.
- **AArch32 (ARMv7-A)**: ARM e Thumb-2 com blocos IT, multiplicações/divisão/saturação/SIMD
  paralelo, LDREX/STREX, bancos de registradores por modo, MMU de descritores curtos (seções,
  páginas, domínios, AFE), CP15, VFPv3-D32 (+VFMA), timer genérico, PSCI.
- **x86 (i386 e x86-64 no mesmo núcleo)**: modo real, protegido 16/32 bits, modo longo e de
  compatibilidade; paginação 32 bits (PSE), PAE e 4 níveis com bits A/D, NX e WP; segmentação,
  TSS e troca de pilha por privilégio, IST, IRET/RETF entre anéis, SYSCALL/SYSRET,
  SYSENTER/SYSEXIT, CPUID, MSRs, TSC; flags preguiçosas; strings com REP (caminho rápido);
  x87 (registradores em precisão dupla, formato de 80 bits na memória); SSE, SSE2, SSE3, SSSE3,
  SSE4.1, SSE4.2 (incluindo PCMPxSTRx e CRC32), POPCNT e MMX com regras de NaN do x86;
  FXSAVE/FXRSTOR.
- **JIT x86 → AArch64** (`x86_jit.c`, `a64_asm.h`; só em hosts AArch64): traduz blocos de uma
  página com TLB em linha, flags preguiçosas no formato do interpretador e eliminação de flags
  mortas; registradores do convidado mantidos em registradores do host dentro do bloco;
  deslocamentos, rotações, MUL/DIV, BT*, BSF/BSR, CMPXCHG/XADD, movimentos e lógica SSE (NEON)
  em código nativo; encadeamento direto de blocos, cache de saltos por ponto de salto e global
  (validados pelo TLB, sobrevivem a trocas de CR3). Código automodificável é detectado por um
  bitmap de páginas de código. Instruções raras (sistema, x87, SSE aritmético, MOV CR8) rodam no
  interpretador de dentro do bloco.
- **Depuração**: `MVM_JIT=0` desliga o JIT; `MVM_JIT_STATS=1` mostra estatísticas (instruções
  interpretadas, motivos de saída dos blocos); `MVM_JIT_OFF=bits` desliga otimizações para
  bissecção; `MVM_JIT_SKIP=op,...` e `MVM_JIT_MAXINSN`; `MVM_PROF=arq` grava um perfil por
  amostragem do host (relatório: `tools/prof_report.py arq [N] [blocos]`, com
  `MVM_JIT_BLOCKS=blocos` para atribuir o tempo aos blocos/módulos do convidado);
  `MVM_DBG_DUMPDIR=dir` grava imagens PE do convidado vistas na pilha; `MVM_PC_DEBUG=N`
  despeja registradores, código e pilha (simbolizada por módulo PE) a cada N s.

**Dispositivos** (`core/src/dev/`): GICv2, PL011, PL031, virtio-mmio (v2), 8259 mestre/escravo com
ELCR, 8254 (canais 0 e 2, porta 0x61), RTC/CMOS, UART 16550A, controlador i8042 (teclado e mouse
PS/2), barramento PCI (mecanismo #1), virtio-pci legado, virtio-blk, framebuffer linear XRGB8888
(`simple-framebuffer` no ARM, VLFB via `screen_info` no x86).

**Tempo**: os temporizadores do convidado (TSC, PIT, APIC, timer genérico do ARM)
seguem um único relógio (o RTC mostra a hora de parede do host). Quando o emulador não acompanha o tempo real, esse relógio é freado
em vez de perder interrupções de timer (`mvm_clock_lag_ns` / `getClockLagNanos()` informam
quanto). No x86 o TSC é de 1 GHz, invariante e com frequência informada pelo CPUID (folhas
0x15/0x16); o Linux o usa como clocksource sem calibrar nem vigiar com o watchdog.

**ACPI** (`core/src/dev/acpi.c`, `core/src/acpi/dsdt.asl`): o DSDT é escrito em ASL e
compilado por `core/tools/gen_dsdt.sh` (precisa do `iasl`) para `dsdt_aml.h`, que fica no
repositório; o build normal não depende do `iasl`. `MVM_ACPI_DUMP=arquivo` grava as tabelas
instaladas no boot direto (para conferir com `iasl -d`).

**Outros dispositivos do PC** (`core/src/dev/`): controlador SATA AHCI 1.2 (ICH9, 6 portas,
discos e CD-ROM ATAPI, com o núcleo de comandos compartilhado com o IDE); HPET com 3 timers e
modo legado; RTC MC146818 completo (interrupções periódica, de alarme e de atualização, bit
UIP); troca de tarefa por hardware do x86 (task gates, usada pelo NT no double fault).

**PC com BIOS** (`core/src/dev/`): APIC local (timer, TPR/PPR, virtual wire) e IOAPIC de 24 pinos;
i440FX + PIIX3 (roteamento PIRQ); `fw_cfg` compatível com o do QEMU (RAM, e820, ordem de boot,
DMA); VGA padrão (`1234:1111`) com modo texto, planar, 256 cores, VBE/Bochs DISPI até
2560x1600x32 e ROM de vídeo; IDE PIIX3 com ATA (LBA28/48, PIO e DMA por bus master) e
CD-ROM ATAPI.

**Carregadores** (`core/src/loader.c`, `core/src/machine/`): bzImage (protocolo de boot do Linux),
zImage, Image/Image.gz/EFI zboot (ARM64), ELF32/ELF64, binários brutos, BIOS; gerador de device
tree; inflate (gzip) próprio.

## Estrutura

```
core/                 núcleo portável em C (sem dependências externas)
  include/mvm.h       API pública em C
  src/                CPUs, dispositivos, máquinas, carregadores
  tools/mvm_cli.c     CLI de host (console serial no terminal)
android/              projeto Gradle
  multivm/            biblioteca Android com.multivm.core (.aar)
    src/main/cpp/     ponte JNI + CMake
    src/main/java/    API Java
  app/                app de teste com.multivm.app (interface simples -> APK)
  build-native.sh     gera a .so sem NDK (clang com alvo Android, ex.: Termux)
tests/
  guest/              programas bare-metal de teste para as 4 arquiteturas
  fuzz/               fuzzer diferencial x86 (referência: qemu-user)
  linux/              boot de Linux real + teste de disco
tools/                utilitários (initramfs, extração de módulos)
```

## Compilar

### CLI de host (Linux/macOS/Termux)

```sh
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release
cmake --build core/build -j
```

### Biblioteca Android

Com Android Studio / NDK (recomendado) — o Gradle compila o C via CMake para `arm64-v8a` e
`x86_64`:

```sh
cd android
./gradlew :multivm:assembleRelease      # -> multivm/build/outputs/aar/multivm-release.aar
```

Sem NDK (por exemplo, compilando no próprio celular com o clang do Termux), gere a `.so` antes:

```sh
cd android
./build-native.sh                       # -> multivm/src/main/jniLibs/arm64-v8a/libmultivm.so
./gradlew :multivm:assembleRelease -Pmultivm.nativeBuild=prebuilt
```

Somente ABIs de 64 bits são suportadas (o núcleo usa `__int128`).

## Usar

### CLI

```sh
# ARM64 com kernel Linux, initramfs e disco
mvm-cli -a arm64 -m 512 -k vmlinuz -i initramfs -d disco.img -c "console=ttyAMA0"

# x86-64 (bzImage)
mvm-cli -a x86_64 -m 512 -k bzImage -i initramfs -c "console=ttyS0"

# ARMv7 (zImage) e i386
mvm-cli -a arm -k zImage -i initramfs -c "console=ttyAMA0"
mvm-cli -a i386 -k bzImage -i initramfs -c "console=ttyS0"
```

Opções úteis: `--fb 800x600` (framebuffer), `--fb-dump tela.ppm`, `-r imagem` (disco somente
leitura), `--dtb`, `--bios`, `-t segundos`, `-e texto` (termina quando o texto aparecer), `-v`.
`Ctrl-A X` sai, `Ctrl-A R` reinicia, `Ctrl-A P` aperta o botão de energia ACPI (x86).

Boot por BIOS (x86), com o SeaBIOS do Debian/Ubuntu (`apt install seabios`):

```sh
# ISO em CD-ROM; o texto e digitado no prompt "boot:" do ISOLINUX
mvm-cli -a x86_64 -m 512 --bios /usr/share/seabios/bios-256k.bin --cdrom alpine.iso \
    --type-after 'boot:' --type '0:virt console=ttyS0\n' --text

# disco rigido IDE (a ordem de boot usa as letras do QEMU: c = disco, d = CD)
mvm-cli -a x86_64 --bios bios-256k.bin -d disco.img --boot c
```

### Imagens de disco

Os discos (`-d`/`-r`, em IDE, AHCI ou virtio-blk, no CLI e no Android) aceitam vários formatos,
reconhecidos pelo conteúdo do arquivo (`core/src/img/`). Todos são lidos e gravados, exceto onde
indicado:

| Formato | Origem | Observações |
|---|---|---|
| raw / ISO | qualquer | TRIM abre buracos no arquivo quando o sistema de arquivos permite |
| qcow2 v2/v3 | QEMU | clusters de 512 B a 2 MiB, refcount de 1 a 64 bits, clusters comprimidos (deflate), snapshots internos (vale o estado atual), arquivo base |
| VDI 1.1 | VirtualBox | dinâmico e fixo |
| VMDK | VMware | monolithicSparse, monolithicFlat, twoGbMaxExtent{Sparse,Flat}; streamOptimized só leitura |
| VHD | Virtual PC, Hyper-V | fixo, dinâmico e diferencial (pai pelos localizadores do arquivo) |
| VHDX | Hyper-V | fixo e dinâmico; log pendente é reaplicado ao abrir |
| **MVD** | MultiVM | formato próprio, descrito abaixo |

Não suportados, com mensagem explicando: criptografia; qcow2 com arquivo de dados externo, L2
estendido ou zstd; VDI, VMDK e VHDX diferenciais (snapshots do VirtualBox, VMware e Hyper-V).
Imagens que dependem de outros arquivos (arquivo base, extents de VMDK) precisam ser abertas pelo
caminho, não pelo seletor de arquivos do Android. Uma imagem raw não pode ter uma assinatura de
outro formato gravada pelo convidado no início ou no fim: uma imagem forjada poderia apontar um
"arquivo base" para qualquer arquivo do host (o QEMU tem a mesma proteção).

**MVD (MultiVM Disk)** é o formato próprio, pensado para o celular. É o padrão do app ao criar
discos.

- O arquivo cresce conforme o uso: um disco de 128 GiB vazio ocupa 4 MiB.
- O mapa de blocos (de 256 KiB por padrão; 64 KiB a 4 MiB) fica inteiro na memória: ler e gravar
  nunca consultam metadados no arquivo.
- As alterações do mapa são gravadas em lote, no flush do convidado ou a cada 2 s de escrita, e
  sempre depois de um `fdatasync` dos dados. O mapa nunca aponta para dados não gravados: numa
  queda perde-se só o que o convidado ainda não tinha confirmado, como num disco real com cache.
- Blocos novos vão para o fim do arquivo; o primeiro acesso grava só os dados do convidado, sem
  preencher o bloco inteiro.
- Anuncia TRIM: virtio-blk (discard e write-zeroes) e ATA DATA SET MANAGEMENT no IDE e no AHCI,
  com o disco visto como não rotacional. Blocos descartados ou inteiramente zerados voltam a ficar
  livres e são reaproveitados.
- Compressão LZ4 opcional na conversão (`-c`). A leitura é transparente; um bloco comprimido
  regravado volta a ser normal.
- Pode ser um overlay sobre outra imagem de qualquer formato (`mvm-img snapshot`).

No celular de teste, com escritas de 4 KiB que alocam espaço novo, o MVD fica no nível do raw
(cerca de 17 mil IOPS) e cerca de 3 vezes à frente do qcow2. O arquivo ocupa o mesmo que os
dados, contra 20% a mais no qcow2.

`mvm-img` (gerado junto com o `mvm-cli`) trabalha com qualquer formato suportado:

```sh
mvm-img info disco.vhdx
mvm-img create disco.mvd 64G                 # -b 1M muda o tamanho do bloco
mvm-img convert windowsxp.qcow2 xp.mvd       # -c comprime; -O raw gera raw
mvm-img snapshot xp.mvd xp-teste.mvd         # overlay: o xp.mvd nao muda
mvm-img check xp.mvd                         # le tudo e mostra o uso de espaco
mvm-img compact xp.mvd                       # reescreve sem espaco desperdicado
```

`--vgabios` escolhe a ROM de vídeo (o padrão é `vgabios-stdvga.bin` ao lado da BIOS).
`--text` imprime a tela de texto VGA ao sair. `--type` digita no teclado PS/2 e aceita
`\n` (Enter), `\wN` (espera N s), `\Sc` (Alt+SysRq+c), `\P` (botão de energia), `\kN` (tecla
de código evdev N) e `\dN`/`\uN` (segura/solta a tecla N, ex.: `\d56\k49\u56` = Alt+N).
`--sata` coloca os discos e o CD-ROM no controlador AHCI. `--control FIFO` recebe comandos
durante a execução, um por linha: `type TEXTO`, `text ARQ` (tela de texto), `shot ARQ.ppm`,
`mouse DX DY [BOTOES]`, `click`, `power`, `reset`, `quit` e `media list|insert N ARQ|eject N|add ARQ`.

Variáveis de depuração do x86: `MVM_X86_TRACE=N` (rastro das últimas N instruções),
`MVM_X86_TRACE_STOP=endereço` (grava o rastro ao chegar nele), `MVM_X86_MEMDUMP`,
`MVM_X86_BREAK=endereço` (com o JIT ligado: registradores e, com `MVM_RAMDUMP=arq`, a RAM inteira),
`MVM_X86_V86LOG=N` (instruções em modo V86, ex.: chamadas de BIOS de vídeo do Windows),
`MVM_X86_DBGPRINT=1` (mostra o DbgPrint de kernels NT), `MVM_X86_SAMPLE=N`,
`MVM_PC_DEBUG=N` (estado de CPU/PIC/PIT/APIC/IDE/VGA a cada N s) e `MVM_I8042_DEBUG=1`.

### Java (Android)

```java
VmConfig cfg = VmConfig.builder(Architecture.ARM64)
        .ramMb(512)
        .kernel(dir + "/vmlinuz")
        .initrd(dir + "/initramfs")
        .cmdline("console=ttyAMA0")
        .addDisk(DiskImage.fromPath(dir + "/disco.img", false))
        .framebuffer(800, 600)
        .build();

VirtualMachine vm = VirtualMachine.create(cfg);
vm.addSerialListener(bytes -> handler.post(() -> terminal.append(bytes)));   // thread da VM
vm.addStateListener((v, state, reason) -> Log.i("VM", state + " " + reason));
vm.start();

vm.sendSerial("uname -a\n");
vm.sendKey(KeyEvent.KEYCODE_A, true);                  // teclado PS/2 (x86)
vm.copyFramebuffer(bitmap);                            // Bitmap ARGB_8888
...
vm.close();                                            // para, espera e libera
```

Discos escolhidos pelo usuário (Storage Access Framework) podem ser passados com
`DiskImage.fromFileDescriptor(pfd, readOnly)`. Qualquer formato de imagem suportado serve.
`DiskImages` cria discos MVD, converte imagens e informa formato e tamanhos. As chamadas são
síncronas: use fora da thread de UI.

```java
DiskImages.createMvd(dir + "/disco.mvd", 64L << 30);
DiskImages.convert(pfd, dir + "/xp.mvd", DiskImages.Format.MVD, false,
        (feito, total) -> { publicarProgresso(feito, total); return !cancelado; });
DiskImages.Info i = DiskImages.info(dir + "/xp.mvd");   // formato, virtualSize, fileSize
```

Boot por BIOS no Android:

```java
VmConfig cfg = VmConfig.builder(Architecture.X86_64)
        .ramMb(512)
        .firmware(dir + "/bios-256k.bin")          // vgabios-stdvga.bin no mesmo diretorio
        .addDisk(DiskImage.cdrom(dir + "/alpine.iso"))
        .addDisk(DiskImage.fromPath(dir + "/disco.img", false, DiskImage.Type.IDE))
        .bootOrder("dc")
        .build();
...
String tela = vm.getTextScreen();                      // modo texto (BIOS, boot loader)
vm.pressPowerButton();                                // desligamento ordenado via ACPI
FramebufferInfo fi = vm.getFramebufferInfo();          // o tamanho muda com o modo de video
```

### App de teste (APK)

`android/app` é uma interface simples para testar o emulador: escolhe a arquitetura, a memória,
boot por BIOS (SeaBIOS embutido no APK) ou por kernel Linux, ISO, disco (pelo seletor de arquivos
do Android, criando um disco MVD novo ou convertendo uma imagem qcow2/VDI/VMDK/VHD/VHDX para MVD,
com barra de progresso) e mostra a tela da VM, um terminal serial e uma barra
de teclas (Esc, Ctrl/Alt/Shift/Win presos, setas, F1–F12, Ctrl+Alt+Del). O toque funciona como
touchpad: arrastar move o mouse, toque = clique, dois dedos = clique direito / rolagem, segurar e
arrastar = arrastar. Teclado e mouse físicos também funcionam.

```sh
cd android
./build-native.sh
./gradlew :app:assembleRelease -Pmultivm.nativeBuild=prebuilt   # -> app/build/outputs/apk/release/app-release.apk
```

O APK de release é assinado com a chave de debug (é só para testes).

### C

Veja `core/include/mvm.h`: `mvm_config_init`, `mvm_create`, `mvm_run` (bloqueia na thread
atual), `mvm_request_stop`, `mvm_serial_input`, `mvm_fb_copy`, `mvm_key_event` etc.

## Testes

```sh
tests/run_guest_tests.sh          # CPU + sistema nas 4 arquiteturas (clang + ld.lld)
tests/fuzz/run_x86.sh 64 1 3000   # fuzzer diferencial x86-64 (precisa de qemu-x86_64)
tests/fuzz/run_x86.sh 32 1 3000   # idem para i386 (qemu-i386)
FUZZ_KIND=sse FUZZ_SSEOP=sse4 tests/fuzz/run_x86.sh 64 1   # so SSE3/SSSE3/SSE4.x
tests/linux/fetch_images.sh       # baixa kernels Alpine e busybox estático
tests/linux/run_linux_tests.sh    # boota Linux nas 4 arquiteturas e testa o disco (raw, qcow2, MVD + TRIM)
tests/img/run_img_tests.sh        # formatos de imagem contra o qemu-img (leitura, escrita, check, quedas)
tools/pdb_publics.py              # simbolos publicos de PDBs antigos (MSF 7.00, ex.: Windows XP)
tests/guest/build_bench.sh        # benchmark de CPU (out/bench-*.elf; imprime MIPS ao final)
tools/prof_report.py PERFIL       # relatorio de MVM_PROF (perfil do host por amostragem)
tools/pdb_syms.py / pe_pdb_url.py # simbolos publicos de binarios do Windows (servidor da Microsoft)
```

- Os **testes convidados** rodam um programa C (inteiros, memória, SHA-256, laços
  vetorizados, ponto flutuante) compilado para cada arquitetura com `-O3` e comparam a saída
  com a mesma compilação nativa; em seguida testam exceções, MMU, modo usuário, syscalls e
  interrupções de timer.
- O **fuzzer diferencial** gera milhares de instruções x86 aleatórias (inteiras, bits,
  strings, SSE até SSE4.2) com estado aleatório e compara registradores, flags definidas, memória
  e registradores XMM com o `qemu-user` (o mesmo programa roda pelo JIT, então ele também é
  testado). Divergência conhecida: com dois NaN o QEMU aplica a
  regra do x87 ao SSE; o MultiVM segue o manual da Intel (primeiro operando).

## Próximos passos

1. **Windows no x86**: desempenho (DWM/WARP e criptografia dominam o tempo: SSE de ponto
   flutuante e mais instruções no JIT); vídeo com driver próprio (WDDM/virtio-gpu); MSI no AHCI;
   depois UEFI (OVMF) para Windows 11 x64 e Windows on ARM.
2. **Desempenho**: SSE aritmético e x87 no JIT, mais instruções de sistema no bloco, JIT para
   os convidados ARM.
3. **Rede e acesso remoto**: IPv6 no NAT; mouse absoluto (tablet USB/virtio) para o VNC.
4. **Entrada no ARM**: virtio-input (teclado/mouse) e virtio-gpu.
5. SMP, NEON no ARMv7, snapshots da VM inteira; imagens diferenciais de VDI/VMDK/VHDX e zstd no qcow2.
6. Interface de usuário Android completa (já existe um app de teste simples em `android/app`).

## Licença

MultiVM é software livre: GNU General Public License versão 3 ou posterior
(`SPDX-License-Identifier: GPL-3.0-or-later`), texto em [`LICENSE`](LICENSE).

O APK inclui o SeaBIOS e o VGA BIOS (LGPLv3) sem modificações; detalhes e links para o
código-fonte em [`THIRD_PARTY.md`](THIRD_PARTY.md). Imagens de sistemas operacionais
(Windows, ISOs, discos) não fazem parte do projeto: use as suas.
