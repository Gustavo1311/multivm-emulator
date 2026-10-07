/* APIC local e IOAPIC. */
#ifndef MVM_APIC_H
#define MVM_APIC_H

#include "devices.h"

#define IOAPIC_PINS 24
#define LAPIC_MMIO_BASE 0xfee00000ULL
#define IOAPIC_MMIO_BASE 0xfec00000ULL

typedef struct lapic lapic;
typedef struct ioapic ioapic;

extern const mvm_io_ops lapic_mmio_ops;  /* 4 KiB */
extern const mvm_io_ops ioapic_mmio_ops; /* 0x20 bytes */

lapic *lapic_new(mvm_vm *vm, void *cpu, void (*set_intr)(void *cpu, int level), i8259 *pic);
void lapic_set_pic(lapic *a, i8259 *pic);
void lapic_free(lapic *a);
void lapic_debug_dump(lapic *a);
void lapic_reset(lapic *a);
/* saida INTR do 8259 (entrada LINT0 / ExtINT) */
void lapic_pic_intr(void *opaque, int level);
/* reconhecimento pela CPU: vetor a executar (ou -1) */
int lapic_ack(lapic *a);
void lapic_deliver(lapic *a, int vector, bool level);
void lapic_set_eoi_cb(lapic *a, void (*cb)(void *opaque, int vector), void *opaque);
/* ganchos da CPU: MSR IA32_APIC_BASE e CR8 */
uint64_t lapic_get_base(void *opaque);
void lapic_set_base(void *opaque, uint64_t v);
uint8_t lapic_get_tpr(void *opaque);
void lapic_set_tpr(void *opaque, uint8_t tpr);

ioapic *ioapic_new(mvm_vm *vm, lapic *a);
void ioapic_free(ioapic *io);
void ioapic_reset(ioapic *io);
void ioapic_set_irq(void *opaque, int pin, int level);

#endif
