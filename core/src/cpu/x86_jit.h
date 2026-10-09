/* JIT x86 -> AArch64 (blocos basicos, com o interpretador como reserva). */
#ifndef MVM_X86_JIT_H
#define MVM_X86_JIT_H

#include "cpu_x86.h"

struct x86_jit;
/* NULL se o host nao suporta JIT ou se MVM_JIT=0 */
struct x86_jit *x86_jit_new(x86_cpu *c);
void x86_jit_free(struct x86_jit *j);
/* descarta todo o codigo traduzido */
void x86_jit_flush(struct x86_jit *j);
/* Executa blocos traduzidos a partir de RIP. Retorna as instrucoes executadas,
 * ou -1 se a instrucao em RIP deve ser executada pelo interpretador. */
int64_t x86_jit_run(x86_cpu *c, int64_t budget);
/* estatisticas (para depuracao) */
void x86_jit_stats(struct x86_jit *j);
void x86_jit_note_interp(x86_cpu *c);
/* o TLB foi invalidado: esquece o cache de saltos indiretos */
void x86_jit_tlb_flushed(struct x86_jit *j);

uint8_t *x86_code_host(x86_cpu *c, uint64_t lin, uint64_t *pa);
bool x86_peek_code(x86_cpu *c, uint64_t lin, void *out, size_t n);
uint64_t x86_read_slow(x86_cpu *c, uint64_t lin, unsigned size, int user);

#endif
