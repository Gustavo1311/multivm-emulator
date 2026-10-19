# Componentes de terceiros

O código do MultiVM (núcleo em C, biblioteca Java/JNI e app) é próprio e distribuído
sob a GPLv3 ou posterior (veja `LICENSE`). Os itens abaixo vêm de outros projetos.

## Incluídos no repositório e no APK

| Arquivo | Projeto | Licença | Código-fonte |
|---|---|---|---|
| `android/app/src/main/assets/bios/bios-256k.bin` | SeaBIOS 1.16.3 (pacote Debian `seabios` 1.16.3-2) | LGPLv3 | https://www.seabios.org/ · https://sources.debian.org/src/seabios/1.16.3-2/ |
| `android/app/src/main/assets/bios/vgabios-stdvga.bin` | VGA BIOS do SeaBIOS 1.16.3 (mesmo pacote) | LGPLv3 | idem |
| `android/app/src/main/assets/bios/OVMF_CODE.fd`, `OVMF_VARS.fd` | OVMF X64 do edk2, tag `edk2-stable202502` (inclui OpenSSL 3, Brotli e outros submódulos do edk2) | BSD-2-Clause-Patent (edk2); Apache-2.0 (OpenSSL); MIT (Brotli) | https://github.com/tianocore/edk2/tree/edk2-stable202502 |
| `android/app/src/main/assets/bios/AAVMF_CODE.fd`, `AAVMF_VARS.fd`, `AAVMF32_CODE.fd`, `AAVMF32_VARS.fd` | edk2 ArmVirtQemu (AARCH64 e ARM), mesma tag `edk2-stable202502` | BSD-2-Clause-Patent (edk2) e licenças dos submódulos | idem |

Os binários do SeaBIOS não foram modificados. O texto da LGPLv3 está em
https://www.gnu.org/licenses/lgpl-3.0.html. O OVMF foi compilado sem alterações no código, com
`tools/build_ovmf.sh` (RELEASE, sem SMM, Secure Boot, TPM, TLS e HTTP boot), e o ArmVirtQemu com
`tools/build_aavmf.sh` (RELEASE, sem Secure Boot, TPM, TLS, HTTP boot e IPv6), com o patch
`tools/edk2-patches/armvirt-virtio-mmio-console.patch` (conecta a virtio-gpu e o teclado virtio-mmio
ao console e inclui o VirtioKeyboardDxe no firmware; mesma licença do edk2); as licenças de cada
componente estão no próprio edk2 (`License.txt` e `License-History.txt` na raiz e nos submódulos).

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
- `tests/uefi/run_uefi_tests.sh` usa o live CD do SliTaz (GPL) e, no primeiro teste, o
  `OVMF.fd` do sistema (`apt install ovmf`).
- `tests/uefi/run_uefi_arm_tests.sh` usa as ISOs "virt" do Alpine 3.24.2 para aarch64 e armv7
  (https://alpinelinux.org/downloads/, GPL e outras licenças livres dos pacotes).
- Os testes com BIOS usam o SeaBIOS do sistema (`/usr/share/seabios`) e as ISOs do Alpine,
  que não fazem parte do repositório.
