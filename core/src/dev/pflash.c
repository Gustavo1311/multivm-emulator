/*
 * Flash paralela CFI (conjunto de comandos Intel/Sharp), como o pflash_cfi01 do QEMU. Guarda o
 * armazenamento de variaveis UEFI.
 *
 * x86 (OVMF): largura de 1 byte e blocos de 4 KiB, logo abaixo do codigo no topo dos 4 GiB. O
 * driver QemuFlashFvbServicesRuntimeDxe usa so:
 *   0x10/0x40 programa um byte, 0x20 + 0xD0 apaga um bloco, 0x50 limpa o status,
 *   0x70 le o status, 0xFF volta ao modo de leitura.
 *
 * ARM (ArmVirtQemu): barramento de 32 bits formado por dois chips de 16 bits (cada comando vem
 * repetido nas duas metades, ex. 0x00FF00FF) e blocos de 256 KiB, em 0x04000000. O VirtNorFlashDxe
 * usa ainda 0xE8 (programa em buffer: contagem, dados, 0xD0), 0x60 (trava/destrava bloco) e 0x90
 * para ler o estado de trava do bloco (sempre destravado aqui).
 *
 * As alteracoes vao para o arquivo quando o convidado volta ao modo de leitura e ao desligar.
 */
#include "pflash.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MODE_ARRAY, MODE_STATUS, MODE_ID, MODE_CFI };

#define BUF_MAX_WORDS 32 /* buffer de programacao do P30: 32 palavras de 32 bits */

struct pflash {
    uint8_t *data;
    uint32_t size;
    int fd;              /* -1 = somente leitura (sem arquivo gravavel) */
    bool ro;
    uint32_t width;      /* largura do barramento: 1 (x86) ou 4 (ARM, 2 chips de 16 bits) */
    uint32_t sector;     /* bloco apagavel */
    int mode;
    uint8_t cmd;         /* comando de varios ciclos pendente (0x10/0x40, 0x20, 0x60, 0xE8), 0 = nenhum */
    int buf_left;        /* 0xE8: palavras de dados ainda esperadas (-1 = esperando a contagem) */
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

/* valor de 8 bits de um chip fora do modo de leitura; idx = endereco do chip (em unidades) */
static uint8_t query(pflash *p, uint32_t idx)
{
    switch (p->mode) {
    case MODE_STATUS:
        return p->status;
    case MODE_ID: /* fabricante Intel, dispositivo 0x18, estado de trava do bloco (0 = destravado) */
        return (idx & 0xff) == 0 ? 0x89 : (idx & 0xff) == 1 ? 0x18 : 0;
    case MODE_CFI: /* so a assinatura "QRY" */
        switch (idx & 0xff) {
        case 0x10: return 'Q';
        case 0x11: return 'R';
        case 0x12: return 'Y';
        default: return 0;
        }
    default:
        return 0;
    }
}

static uint8_t read_byte(pflash *p, uint32_t off)
{
    if (p->mode == MODE_ARRAY)
        return p->data[off];
    if (p->width == 1)
        return query(p, off);
    /* 2 chips de 16 bits: cada um responde no byte baixo da sua metade (ex. status 0x00800080) */
    return (off & 1) ? 0 : query(p, off / 4);
}

static uint64_t pflash_read(void *opaque, uint64_t off, unsigned size)
{
    pflash *p = opaque;
    uint64_t v = 0;
    for (unsigned i = 0; i < size && off + i < p->size; i++)
        v |= (uint64_t)read_byte(p, (uint32_t)(off + i)) << (8 * i);
    return v;
}

static void program(pflash *p, uint32_t off, uint64_t val, unsigned size)
{
    if (p->ro) {
        p->status |= 0x90; /* erro de programacao: protegida contra escrita */
        return;
    }
    if (off + size > p->size)
        size = p->size - off;
    for (unsigned i = 0; i < size; i++)
        p->data[off + i] = (uint8_t)(val >> (8 * i));
    mark_dirty(p, off, size);
}

static void pflash_write(void *opaque, uint64_t off64, uint64_t val, unsigned size)
{
    pflash *p = opaque;
    uint32_t off = (uint32_t)off64;
    uint8_t v = (uint8_t)val; /* comando (no modo de 32 bits, repetido nas duas metades) */
    if (off >= p->size)
        return;
    switch (p->cmd) {
    case 0x10:
    case 0x40: /* segundo ciclo do programa: o dado */
        p->cmd = 0;
        p->mode = MODE_STATUS;
        program(p, off, val, p->width == 1 ? 1 : size);
        p->status |= 0x80;
        return;
    case 0x20: { /* segundo ciclo do apagamento: confirmacao */
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
        uint32_t base = off & ~(p->sector - 1);
        uint32_t len = base + p->sector <= p->size ? p->sector : p->size - base;
        memset(p->data + base, 0xff, len);
        mark_dirty(p, base, len);
        p->status |= 0x80;
        return;
    }
    case 0x60: /* trava (0x01), destrava (0xD0) ou trava permanente (0x2F): sem efeito */
        p->cmd = 0;
        p->mode = MODE_STATUS;
        p->status |= 0x80;
        return;
    case 0xe8: /* programa em buffer: contagem, N palavras, 0xD0 */
        if (p->buf_left < 0) {
            p->buf_left = (int)(val & 0xffff) + 1;
            if (p->buf_left > BUF_MAX_WORDS) {
                p->status |= 0xb0;
                p->cmd = 0;
            }
            return;
        }
        if (p->buf_left > 0) {
            program(p, off, val, size);
            p->buf_left--;
            return;
        }
        p->cmd = 0;
        p->mode = MODE_STATUS;
        if (v != 0xd0)
            p->status |= 0xb0;
        p->status |= 0x80;
        return;
    }
    switch (v) {
    case 0x10:
    case 0x40:
    case 0x20:
    case 0x60:
        p->cmd = v;
        break;
    case 0xe8:
        p->cmd = v;
        p->buf_left = -1;
        p->mode = MODE_STATUS; /* o convidado le o status para ver se o buffer esta livre */
        p->status |= 0x80;
        break;
    case 0x50: /* limpa o status (0, como no QEMU: o OVMF detecta a flash por isso) e volta ao modo de leitura */
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

pflash *pflash_open_ex(const char *path, uint32_t size_limit, uint32_t width, uint32_t sector, char *err,
                       size_t errlen)
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
    if (len <= 0 || len > (off_t)size_limit || (len & 0xfff)) {
        snprintf(err, errlen, "tamanho invalido do arquivo de variaveis UEFI (%lld)", (long long)len);
        close(fd);
        return NULL;
    }
    pflash *p = calloc(1, sizeof(*p));
    p->size = (uint32_t)len;
    p->width = width;
    p->sector = sector;
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

pflash *pflash_new_blank(uint32_t size, uint32_t width, uint32_t sector)
{
    pflash *p = calloc(1, sizeof(*p));
    p->size = size;
    p->width = width;
    p->sector = sector;
    p->fd = -1;
    p->data = malloc(size);
    memset(p->data, 0xff, size);
    pflash_reset(p);
    return p;
}

pflash *pflash_open(const char *path, uint32_t size_limit, char *err, size_t errlen)
{
    return pflash_open_ex(path, size_limit, 1, 0x1000, err, errlen); /* PcdOvmfFirmwareBlockSize */
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
