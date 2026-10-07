/* Executa o teste nativamente para gerar a saida de referencia. */
#include <stdio.h>

int test_main(void);
void test_putc(char c) { putchar(c); }
int main(void) { return test_main(); }
