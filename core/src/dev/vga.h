/* VGA padrao com extensoes VBE (Bochs DISPI). */
#ifndef MVM_VGA_H
#define MVM_VGA_H

#include "devices.h"

typedef struct vga vga;

vga *vga_new(mvm_vm *vm, uint32_t vram_mb);
void vga_debug_dump(vga *v);
void vga_free(vga *v);
void vga_reset(vga *v);
/* memoria de video para o BAR0 (framebuffer linear) */
uint8_t *vga_vram(vga *v);
uint32_t vga_vram_size(vga *v);
_Atomic uint32_t *vga_vram_gen(vga *v);
uint64_t *vga_vram_dirty(vga *v); /* 1 bit por pagina de 4 KiB da VRAM */
/* texto da tela em modo texto (ASCII); 0 se nao estiver em modo texto */
size_t vga_text(vga *v, char *buf, size_t len);

extern const mvm_io_ops vga_io_ops;  /* portas 0x3B0-0x3DF */
extern const mvm_io_ops vga_vbe_ops; /* portas 0x1CE-0x1CF */
extern const mvm_io_ops vga_mem_ops; /* janela 0xA0000-0xBFFFF */

#endif
