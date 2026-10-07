# Componentes de terceiros

O código do MultiVM (núcleo em C, biblioteca Java/JNI e app) é próprio e distribuído
sob a GPLv3 ou posterior (veja `LICENSE`). Os itens abaixo vêm de outros projetos.

## Incluídos no repositório e no APK

| Arquivo | Projeto | Licença | Código-fonte |
|---|---|---|---|
| `android/app/src/main/assets/bios/bios-256k.bin` | SeaBIOS 1.16.3 (pacote Debian `seabios` 1.16.3-2) | LGPLv3 | https://www.seabios.org/ · https://sources.debian.org/src/seabios/1.16.3-2/ |
| `android/app/src/main/assets/bios/vgabios-stdvga.bin` | VGA BIOS do SeaBIOS 1.16.3 (mesmo pacote) | LGPLv3 | idem |

Os binários não foram modificados. O texto da LGPLv3 está em
https://www.gnu.org/licenses/lgpl-3.0.html.

## Usados só em tempo de execução (não incluídos)

- **zlib**: carregada da biblioteca do sistema (`libz.so`) com `dlopen`; nenhum código da
  zlib está no repositório.
- **Codificador JPEG** (`core/src/jpeg_enc.c`): implementação própria; as tabelas de
  quantização e Huffman são as do padrão ITU-T T.81, Anexo K.

## Baixados pelos scripts de teste (não incluídos)

- `tests/linux/fetch_images.sh`: kernel Linux e initramfs do Alpine Linux (netboot),
  módulos extraídos dele e o busybox estático. Licenças: GPLv2 (Linux, busybox) e as de
  cada pacote do Alpine. Origem: https://dl-cdn.alpinelinux.org/
- `tests/audio/run_audio_tests.sh`: pacotes do Alpine 3.24 listados em
  `tests/audio/apks.txt` (alsa-utils e dependências).
- Os testes com BIOS usam o SeaBIOS do sistema (`/usr/share/seabios`) e as ISOs do Alpine,
  que não fazem parte do repositório.
