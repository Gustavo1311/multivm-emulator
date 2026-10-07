/* HPET (0xFED00000). */
#ifndef MVM_HPET_H
#define MVM_HPET_H

#include "devices.h"

#define HPET_MMIO_BASE 0xfed00000ULL
#define HPET_TIMERS 3
/* linhas especiais passadas a set_irq no modo legado (as demais sao pinos do IOAPIC) */
#define HPET_LINE_IRQ0 -1
#define HPET_LINE_IRQ8 -2

typedef struct hpet hpet;
extern const mvm_io_ops hpet_mmio_ops; /* 0x400 bytes */

hpet *hpet_new(mvm_vm *vm, void (*set_irq)(void *opaque, int line, int level), void *opaque);
void hpet_free(hpet *h);
void hpet_reset(hpet *h);
/* modo legado ativo: o PIT e o RTC ficam desligados das IRQs 0 e 8 */
bool hpet_legacy(const hpet *h);
/* "Event Timer Block ID" da tabela ACPI HPET */
uint32_t hpet_block_id(void);

#endif
