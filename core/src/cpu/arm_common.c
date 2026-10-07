/* Timer generico ARM, PSCI e conversoes de meia precisao. */
#include "arm_common.h"

#include <math.h>

static void gt_cb(void *opaque);

void gt_init(arm_gtimer *g, mvm_vm *vm)
{
    memset(g, 0, sizeof(*g));
    g->vm = vm;
    for (int i = 0; i < 2; i++) {
        g->ctx[i].g = g;
        g->ctx[i].i = i;
        timer_init(&g->t[i], gt_cb, &g->ctx[i]);
    }
}

void gt_reset(arm_gtimer *g)
{
    for (int i = 0; i < 2; i++) {
        timer_del(g->vm, &g->t[i]);
        g->ctl[i] = 0;
        g->cval[i] = 0;
        if (g->out[i] && g->irq)
            g->irq(g->opaque, i, 0);
        g->out[i] = 0;
    }
    g->cntvoff = 0;
    g->cntkctl = 0;
}

uint64_t gt_cntpct(arm_gtimer *g)
{
    int64_t ns = mvm_now(g->vm);
    if (ns < 0)
        ns = 0;
    return (uint64_t)((unsigned __int128)ns * GT_FREQ / 1000000000ULL);
}

static uint64_t gt_count(arm_gtimer *g, int i) { return i == GT_VIRT ? gt_cntvct(g) : gt_cntpct(g); }

void gt_update(arm_gtimer *g, int i)
{
    uint64_t count = gt_count(g, i);
    bool enable = g->ctl[i] & 1, imask = g->ctl[i] & 2;
    bool istatus = count >= g->cval[i];
    int level = enable && istatus && !imask;
    if (level != g->out[i]) {
        g->out[i] = level;
        if (g->irq)
            g->irq(g->opaque, i, level);
    }
    timer_del(g->vm, &g->t[i]);
    if (enable && !istatus) {
        uint64_t target = g->cval[i] + (i == GT_VIRT ? g->cntvoff : 0);
        /* converte contagem fisica -> ns (arredondando para cima) */
        unsigned __int128 ns = ((unsigned __int128)target * 1000000000ULL + GT_FREQ - 1) / GT_FREQ;
        if (ns < (unsigned __int128)INT64_MAX / 2)
            timer_mod(g->vm, &g->t[i], (int64_t)ns);
    }
}

static void gt_cb(void *opaque)
{
    gt_ctx *c = opaque;
    gt_update(c->g, c->i);
}

uint32_t gt_ctl_read(arm_gtimer *g, int i)
{
    uint32_t v = g->ctl[i] & 3;
    if (gt_count(g, i) >= g->cval[i])
        v |= 4;
    return v;
}

void gt_ctl_write(arm_gtimer *g, int i, uint32_t v)
{
    g->ctl[i] = v & 3;
    gt_update(g, i);
}

void gt_cval_write(arm_gtimer *g, int i, uint64_t v)
{
    g->cval[i] = v;
    gt_update(g, i);
}

uint32_t gt_tval_read(arm_gtimer *g, int i) { return (uint32_t)(g->cval[i] - gt_count(g, i)); }

void gt_tval_write(arm_gtimer *g, int i, uint32_t v)
{
    g->cval[i] = gt_count(g, i) + (uint64_t)(int64_t)(int32_t)v;
    gt_update(g, i);
}

int64_t arm_psci_call(mvm_vm *vm, uint32_t fn, uint64_t a1, bool *handled)
{
    *handled = true;
    switch (fn) {
    case PSCI_VERSION:
        return 0x10000; /* PSCI 1.0 */
    case PSCI_MIGRATE_INFO_TYPE:
        return 2; /* sem migracao necessaria */
    case PSCI_SYSTEM_OFF:
        LOGI("PSCI: SYSTEM_OFF");
        vm_request_shutdown(vm);
        return 0;
    case PSCI_SYSTEM_RESET:
        LOGI("PSCI: SYSTEM_RESET");
        vm_request_guest_reset(vm);
        return 0;
    case PSCI_CPU_OFF:
        vm_request_shutdown(vm);
        return 0;
    case PSCI_CPU_SUSPEND:
        return 0;
    case PSCI_AFFINITY_INFO_32:
    case PSCI_AFFINITY_INFO_64:
        return a1 == 0 ? 0 : -2;
    case PSCI_CPU_ON_32:
    case PSCI_CPU_ON_64:
        return -2; /* INVALID_PARAMETERS: apenas 1 CPU */
    case PSCI_FEATURES:
        switch ((uint32_t)a1) {
        case PSCI_VERSION: case PSCI_CPU_OFF: case PSCI_SYSTEM_OFF: case PSCI_SYSTEM_RESET:
        case PSCI_FEATURES: case PSCI_CPU_SUSPEND: case PSCI_MIGRATE_INFO_TYPE:
        case PSCI_AFFINITY_INFO_32: case PSCI_AFFINITY_INFO_64: case PSCI_CPU_ON_32:
        case PSCI_CPU_ON_64:
            return 0;
        case SMCCC_VERSION:
            return 0;
        default:
            return -1;
        }
    case SMCCC_VERSION:
        return 0x10001; /* SMCCC 1.1 */
    case SMCCC_ARCH_FEATURES:
        return -1;
    default:
        *handled = false;
        return -1; /* NOT_SUPPORTED */
    }
}

float f16_to_f32(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float r;
    if (e == 0)
        r = ldexpf((float)m, -24);
    else if (e == 31)
        r = m ? NAN : INFINITY;
    else
        r = ldexpf((float)(m | 0x400), (int)e - 25);
    return s ? -r : r;
}

uint16_t f32_to_f16(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000;
    int e = (int)((x >> 23) & 0xff);
    uint32_t m = x & 0x7fffff;
    if (e == 255)
        return (uint16_t)(s | 0x7c00 | (m ? 0x200 : 0));
    e = e - 127 + 15;
    if (e >= 31)
        return (uint16_t)(s | 0x7c00);
    if (e <= 0) {
        if (e < -10)
            return (uint16_t)s;
        m |= 0x800000;
        unsigned shift = (unsigned)(14 - e);
        uint32_t hm = m >> shift;
        uint32_t rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (hm & 1)))
            hm++;
        return (uint16_t)(s | hm);
    }
    uint32_t hm = m >> 13, rem = m & 0x1fff;
    uint32_t r = s | ((uint32_t)e << 10) | hm;
    if (rem > 0x1000 || (rem == 0x1000 && (hm & 1)))
        r++;
    return (uint16_t)r;
}
