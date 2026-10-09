/* APIC local e IOAPIC. */
#ifndef MVM_APIC_H
#define MVM_APIC_H

#include "devices.h"

#define IOAPIC_PINS 24
#define LAPIC_MMIO_BASE 0xfee00000ULL
#define IOAPIC_MMIO_BASE 0xfec00000ULL

typedef struct lapic lapic;
typedef struct ioapic ioapic;
typedef struct apic_bus apic_bus;

extern const mvm_io_ops lapic_mmio_ops;  /* 4 KiB (opaque = um lapic) */
extern const mvm_io_ops apic_bus_mmio_ops; /* 4 KiB (opaque = barramento: o LAPIC da CPU que acessa) */

/* Varias CPUs: os LAPICs ficam num barramento (IPIs e entrega do IOAPIC por destino).
 * startup(cpu, tipo, vetor): INIT (0), SIPI (1) e NMI (2) pedidos a uma CPU. */
enum { APIC_STARTUP_INIT, APIC_STARTUP_SIPI, APIC_STARTUP_NMI };
apic_bus *apic_bus_new(mvm_vm *vm, void (*startup)(void *cpu, int kind, int vector));
/* acrescenta o LAPIC da proxima CPU (indice = ID do APIC; o 0 e a BSP) */
void apic_bus_add(apic_bus *b, lapic *a);
lapic *apic_bus_get(apic_bus *b, int index);
void apic_bus_free(apic_bus *b);
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
/* idem, com as travas (a do APIC; a bql so se for o 8259): para a CPU chamar */
int lapic_intr_ack(lapic *a);
void lapic_deliver(lapic *a, int vector, bool level);
void lapic_set_eoi_cb(lapic *a, void (*cb)(void *opaque, int vector), void *opaque);
/* ganchos da CPU: MSR IA32_APIC_BASE e CR8 */
uint64_t lapic_get_base(void *opaque);
void lapic_set_base(void *opaque, uint64_t v);
uint8_t lapic_get_tpr(void *opaque);
void lapic_set_tpr(void *opaque, uint8_t tpr);

ioapic *ioapic_new(mvm_vm *vm, apic_bus *b);
void ioapic_free(ioapic *io);
void ioapic_reset(ioapic *io);
void ioapic_set_irq(void *opaque, int pin, int level);

#endif
