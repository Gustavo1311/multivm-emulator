/*
 * Flash paralela CFI (conjunto de comandos Intel/Sharp, largura de 1 byte), como o pflash_cfi01 do
 * QEMU. Guarda o armazenamento de variaveis UEFI do OVMF (OVMF_VARS.fd) logo abaixo do codigo, no
 * topo dos 4 GiB. O driver QemuFlashFvbServicesRuntimeDxe do OVMF usa so:
 *   0x10/0x40 programa um byte, 0x20 + 0xD0 apaga um bloco, 0x50 limpa o status,
 *   0x70 le o status, 0xFF volta ao modo de leitura.
 * As alteracoes vao para o arquivo quando o convidado volta ao modo de leitura e ao desligar.
 */
#include "pflash.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PFLASH_SECTOR 0x1000 /* bloco apagavel (PcdOvmfFirmwareBlockSize) */

enum { MODE_ARRAY, MODE_STATUS, MODE_ID, MODE_CFI };

struct pflash {
    uint8_t *data;
    uint32_t size;
    int fd;              /* -1 = somente leitura (sem arquivo gravavel) */
    bool ro;
    int mode;
    uint8_t cmd;         /* comando de 2 ciclos pendente (0x10/0x40 ou 0x20), 0 = nenhum */
    uint8_t status;      /* bit 7 = pronto, bit 5 = erro de apagamento, bit 4 = erro de programacao */
    uint32_t dirty_lo, dirty_hi; /* faixa alterada ainda nao gravada (lo >= hi = nada) */
};

static void mark_dirty(pflash *p, uint32_t off, uint32_t len)
{
    if (p->dirty_lo >= p->dirty_hi) {
        p->dirty_lo = off;
        p->dirty_hi = off + len;
        return;
    }
    if (off < p->dirty_lo) p->dirty_lo = off;
    if (off + len > p->dirty_hi) p->dirty_hi = off + len;
}

void pflash_flush(pflash *p)
{
    if (!p || p->fd < 0 || p->dirty_lo >= p->dirty_hi)
        return;
    uint32_t off = p->dirty_lo, len = p->dirty_hi - p->dirty_lo;
    while (len) {
        ssize_t n = pwrite(p->fd, p->data + off, len, off);
        if (n <= 0) {
            LOGW("pflash: falha ao gravar as variaveis UEFI");
            break;
        }
        off += (uint32_t)n;
        len -= (uint32_t)n;
    }
    p->dirty_lo = p->dirty_hi = 0;
}

static uint8_t read_byte(pflash *p, uint32_t off)
{
    switch (p->mode) {
    case MODE_STATUS:
        return p->status;
    case MODE_ID: /* fabricante Intel, dispositivo 0x18 */
        return (off & 0xff) == 0 ? 0x89 : (off & 0xff) == 1 ? 0x18 : 0;
    case MODE_CFI: /* so a assinatura "QRY" */
        switch (off & 0xff) {
        case 0x10: return 'Q';
        case 0x11: return 'R';
        case 0x12: return 'Y';
        default: return 0;
        }
    default:
        return p->data[off];
    }
}

static uint64_t pflash_read(void *opaque, uint64_t off, unsigned size)
{
    pflash *p = opaque;
    uint64_t v = 0;
    for (unsigned i = 0; i < size && off + i < p->size; i++)
        v |= (uint64_t)read_byte(p, (uint32_t)(off + i)) << (8 * i);
    return v;
}

static void pflash_write(void *opaque, uint64_t off64, uint64_t val, unsigned size)
{
    pflash *p = opaque;
    uint32_t off = (uint32_t)off64;
    uint8_t v = (uint8_t)val;
    (void)size;
    if (off >= p->size)
        return;
    if (p->cmd == 0x10 || p->cmd == 0x40) { /* segundo ciclo do programa: o dado */
        p->cmd = 0;
        p->mode = MODE_STATUS;
        if (p->ro) {
            p->status |= 0x90; /* erro de programacao: protegida contra escrita */
            return;
        }
        p->data[off] = v;
        mark_dirty(p, off, 1);
        p->status |= 0x80;
        return;
    }
    if (p->cmd == 0x20) { /* segundo ciclo do apagamento: confirmacao */
        p->cmd = 0;
        p->mode = MODE_STATUS;
        if (v != 0xd0) {
            p->status |= 0xb0; /* sequencia invalida */
            return;
        }
        if (p->ro) {
            p->status |= 0xa0;
            return;
        }
        uint32_t base = off & ~(uint32_t)(PFLASH_SECTOR - 1);
        memset(p->data + base, 0xff, PFLASH_SECTOR);
        mark_dirty(p, base, PFLASH_SECTOR);
        p->status |= 0x80;
        return;
    }
    switch (v) {
    case 0x10:
    case 0x40:
    case 0x20:
        p->cmd = v;
        break;
    case 0x50: /* limpa o status e volta ao modo de leitura */
        p->status = 0;
        p->mode = MODE_ARRAY;
        break;
    case 0x70:
        p->mode = MODE_STATUS;
        break;
    case 0x90:
        p->mode = MODE_ID;
        break;
    case 0x98:
        p->mode = MODE_CFI;
        break;
    case 0xff:
    case 0x00:
        p->mode = MODE_ARRAY;
        pflash_flush(p);
        break;
    default:
        LOGD("pflash: comando 0x%02x desconhecido em 0x%x", v, off);
        p->mode = MODE_ARRAY;
        break;
    }
}

const mvm_io_ops pflash_ops = {pflash_read, pflash_write};

pflash *pflash_open(const char *path, uint32_t size_limit, char *err, size_t errlen)
{
    int fd = open(path, O_RDWR);
    bool ro = false;
    if (fd < 0) {
        fd = open(path, O_RDONLY);
        ro = true;
    }
    if (fd < 0) {
        snprintf(err, errlen, "nao foi possivel abrir as variaveis UEFI '%s'", path);
        return NULL;
    }
    off_t len = lseek(fd, 0, SEEK_END);
    if (len <= 0 || len > (off_t)size_limit || (len & (PFLASH_SECTOR - 1))) {
        snprintf(err, errlen, "tamanho invalido do arquivo de variaveis UEFI (%lld)", (long long)len);
        close(fd);
        return NULL;
    }
    pflash *p = calloc(1, sizeof(*p));
    p->size = (uint32_t)len;
    p->data = malloc(p->size);
    if (pread(fd, p->data, p->size, 0) != (ssize_t)p->size) {
        snprintf(err, errlen, "falha ao ler as variaveis UEFI '%s'", path);
        free(p->data);
        free(p);
        close(fd);
        return NULL;
    }
    p->ro = ro;
    p->fd = ro ? -1 : fd;
    if (ro) {
        close(fd);
        LOGW("variaveis UEFI somente leitura: alteracoes do convidado serao recusadas");
    }
    pflash_reset(p);
    return p;
}

uint32_t pflash_size(pflash *p) { return p->size; }

void pflash_reset(pflash *p)
{
    pflash_flush(p);
    p->mode = MODE_ARRAY;
    p->cmd = 0;
    p->status = 0x80;
}

void pflash_free(pflash *p)
{
    if (!p)
        return;
    pflash_flush(p);
    if (p->fd >= 0) {
        fsync(p->fd);
        close(p->fd);
    }
    free(p->data);
    free(p);
}
