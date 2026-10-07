/* Espacos de enderecamento: RAM/ROM mapeadas diretamente e regioes de MMIO. */
#include "internal.h"

#include <stdlib.h>

static void space_changed(mvm_space *s)
{
    mvm_vm *vm = s->vm;
    if (vm && s == &vm->mem && vm->cpu && vm->cpu_ops && vm->cpu_ops->tlb_flush)
        vm->cpu_ops->tlb_flush(vm->cpu);
}

static mvm_region *space_add(mvm_space *s, const mvm_region *nr)
{
    if (s->n >= MVM_MAX_REGIONS) {
        LOGE("space: regioes demais (%s)", nr->name);
        return NULL;
    }
    /* mantem ordenado por base para a busca binaria */
    int i = s->n;
    while (i > 0 && s->r[i - 1].base > nr->base) {
        s->r[i] = s->r[i - 1];
        i--;
    }
    s->r[i] = *nr;
    s->n++;
    s->last = 0;
    space_changed(s);
    return &s->r[i];
}

mvm_region *space_add_ram(mvm_space *s, uint64_t base, uint64_t size, uint8_t *host,
                          bool readonly, const char *name)
{
    mvm_region r = {.base = base, .size = size, .host = host, .readonly = readonly, .name = name};
    return space_add(s, &r);
}

mvm_region *space_add_io(mvm_space *s, uint64_t base, uint64_t size, const mvm_io_ops *ops,
                         void *opaque, const char *name)
{
    mvm_region r = {.base = base, .size = size, .ops = ops, .opaque = opaque, .name = name};
    return space_add(s, &r);
}

mvm_region *space_find(mvm_space *s, uint64_t addr)
{
    mvm_region *r = &s->r[s->last];
    if (s->n && addr - r->base < r->size)
        return r;
    int lo = 0, hi = s->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        r = &s->r[mid];
        if (addr < r->base)
            hi = mid - 1;
        else if (addr - r->base >= r->size)
            lo = mid + 1;
        else {
            s->last = mid;
            return r;
        }
    }
    return NULL;
}

uint64_t space_read(mvm_space *s, uint64_t addr, unsigned size)
{
    mvm_region *r = space_find(s, addr);
    if (!r) {
        LOGD("leitura de endereco nao mapeado 0x%llx (%u)", (unsigned long long)addr, size);
        return size >= 8 ? ~0ULL : ((1ULL << (size * 8)) - 1);
    }
    uint64_t off = addr - r->base;
    if (r->host) {
        if (off + size <= r->size)
            return ld_le(r->host + off, size);
        /* atravessa a borda da regiao: byte a byte */
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= space_read(s, addr + i, 1) << (8 * i);
        return v;
    }
    return r->ops->read ? r->ops->read(r->opaque, off, size) : 0;
}

void space_write(mvm_space *s, uint64_t addr, uint64_t val, unsigned size)
{
    mvm_region *r = space_find(s, addr);
    if (!r) {
        LOGD("escrita em endereco nao mapeado 0x%llx = 0x%llx (%u)", (unsigned long long)addr,
             (unsigned long long)val, size);
        return;
    }
    uint64_t off = addr - r->base;
    if (r->host) {
        if (r->readonly)
            return;
        if (off + size <= r->size) {
            space_code_check(s, addr, size);
            st_le(r->host + off, val, size);
            if (r->dirty_gen)
                atomic_fetch_add_explicit(r->dirty_gen, 1, memory_order_relaxed);
            return;
        }
        for (unsigned i = 0; i < size; i++)
            space_write(s, addr + i, (val >> (8 * i)) & 0xff, 1);
        return;
    }
    if (r->ops->write)
        r->ops->write(r->opaque, off, val, size);
}

uint8_t *space_ram_ptr(mvm_space *s, uint64_t addr, uint64_t len, bool write)
{
    mvm_region *r = space_find(s, addr);
    if (!r || !r->host)
        return NULL;
    if (write && (r->readonly || r->dirty_gen))
        return NULL;
    uint64_t off = addr - r->base;
    if (off + len > r->size)
        return NULL;
    if (write) /* o chamador vai escrever: codigo traduzido nessas paginas fica invalido */
        space_code_check(s, addr, len);
    return r->host + off;
}

void space_memread(mvm_space *s, uint64_t addr, void *buf, uint64_t len)
{
    uint8_t *d = buf;
    while (len) {
        uint64_t chunk = PAGE_SIZE - (addr & (PAGE_SIZE - 1));
        if (chunk > len)
            chunk = len;
        uint8_t *p = space_ram_ptr(s, addr, chunk, false);
        if (p) {
            memcpy(d, p, chunk);
        } else {
            for (uint64_t i = 0; i < chunk; i++)
                d[i] = (uint8_t)space_read(s, addr + i, 1);
        }
        d += chunk;
        addr += chunk;
        len -= chunk;
    }
}

void space_memwrite(mvm_space *s, uint64_t addr, const void *buf, uint64_t len)
{
    const uint8_t *d = buf;
    while (len) {
        uint64_t chunk = PAGE_SIZE - (addr & (PAGE_SIZE - 1));
        if (chunk > len)
            chunk = len;
        uint8_t *p = space_ram_ptr(s, addr, chunk, true);
        if (p) {
            memcpy(p, d, chunk);
        } else {
            for (uint64_t i = 0; i < chunk; i++)
                space_write(s, addr + i, d[i], 1);
        }
        d += chunk;
        addr += chunk;
        len -= chunk;
    }
}

bool space_remove_at(mvm_space *s, uint64_t base)
{
    for (int i = 0; i < s->n; i++) {
        if (s->r[i].base == base) {
            memmove(&s->r[i], &s->r[i + 1], (size_t)(s->n - i - 1) * sizeof(mvm_region));
            s->n--;
            s->last = 0;
            space_changed(s);
            return true;
        }
    }
    return false;
}
