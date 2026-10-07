/* Partes comuns a ARM (AArch32) e ARM64 (AArch64): timer generico, condicoes, PSCI. */
#ifndef MVM_ARM_COMMON_H
#define MVM_ARM_COMMON_H

#include "../internal.h"

#define GT_FREQ 62500000ULL /* 62.5 MHz, como o QEMU */

enum { GT_PHYS = 0, GT_VIRT = 1 };

struct arm_gtimer;
typedef struct gt_ctx {
    struct arm_gtimer *g;
    int i;
} gt_ctx;

typedef struct arm_gtimer {
    mvm_vm *vm;
    uint32_t ctl[2];
    uint64_t cval[2];
    uint64_t cntvoff;
    uint32_t cntkctl;
    mvm_timer t[2];
    int out[2];
    void (*irq)(void *opaque, int which, int level);
    void *opaque;
    gt_ctx ctx[2];
} arm_gtimer;

void gt_init(arm_gtimer *g, mvm_vm *vm);
void gt_reset(arm_gtimer *g);
uint64_t gt_cntpct(arm_gtimer *g);
static inline uint64_t gt_cntvct(arm_gtimer *g) { return gt_cntpct(g) - g->cntvoff; }
uint32_t gt_ctl_read(arm_gtimer *g, int i);
void gt_ctl_write(arm_gtimer *g, int i, uint32_t v);
void gt_cval_write(arm_gtimer *g, int i, uint64_t v);
uint32_t gt_tval_read(arm_gtimer *g, int i);
void gt_tval_write(arm_gtimer *g, int i, uint32_t v);
void gt_update(arm_gtimer *g, int i);

/* Ganchos que a maquina fornece a CPU ARM */
typedef struct {
    void *opaque;
    void (*timer_irq)(void *opaque, int which, int level);
} arm_hooks;

static inline bool arm_cond(unsigned cond, uint32_t nzcv)
{
    bool n = nzcv & 8, z = nzcv & 4, c = nzcv & 2, v = nzcv & 1;
    bool r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = c; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = c && !z; break;
    case 5: r = n == v; break;
    case 6: r = (n == v) && !z; break;
    default: r = true; break;
    }
    if ((cond & 1) && cond != 15)
        r = !r;
    return r;
}

/* PSCI (chamado via HVC/SMC). Retorna o valor para x0/r0. */
#define PSCI_VERSION 0x84000000u
#define PSCI_CPU_SUSPEND 0x84000001u
#define PSCI_CPU_OFF 0x84000002u
#define PSCI_CPU_ON_32 0x84000003u
#define PSCI_AFFINITY_INFO_32 0x84000004u
#define PSCI_MIGRATE_INFO_TYPE 0x84000006u
#define PSCI_SYSTEM_OFF 0x84000008u
#define PSCI_SYSTEM_RESET 0x84000009u
#define PSCI_FEATURES 0x8400000Au
#define PSCI_CPU_ON_64 0xC4000003u
#define PSCI_AFFINITY_INFO_64 0xC4000004u
#define SMCCC_VERSION 0x80000000u
#define SMCCC_ARCH_FEATURES 0x80000001u

int64_t arm_psci_call(mvm_vm *vm, uint32_t fn, uint64_t a1, bool *handled);

/* Conversoes de ponto flutuante de meia precisao */
float f16_to_f32(uint16_t h);
uint16_t f32_to_f16(float f);

/* Construtores */
void *arm64_cpu_new(mvm_vm *vm, const arm_hooks *hooks);
void arm64_set_irq(void *cpu, int line, int level);
void arm64_set_entry(void *cpu, uint64_t pc, uint64_t x0);
extern const mvm_cpu_ops arm64_cpu_ops;

void *arm32_cpu_new(mvm_vm *vm, const arm_hooks *hooks);
void arm32_set_irq(void *cpu, int line, int level);
void arm32_set_entry(void *cpu, uint32_t pc, uint32_t r0, uint32_t r1, uint32_t r2);
extern const mvm_cpu_ops arm32_cpu_ops;

#endif
