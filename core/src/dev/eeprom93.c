/*
 * EEPROM serial 93C46 (Microwire, 64 palavras de 16 bits), so leitura. Usada pela
 * RTL8139 (registrador 9346CR) e pela e1000 (EECD) para entregar o MAC ao driver.
 */
#include "devices.h"

enum { EE_IDLE, EE_CMD, EE_OUT };

void ee93_reset(eeprom93 *e)
{
    e->state = EE_IDLE;
    e->bits = 0;
    e->shift = 0;
    e->dout = true;
}

void ee93_write(eeprom93 *e, bool cs, bool sk, bool di)
{
    bool rise = sk && !e->sk;
    e->sk = sk;
    if (!cs) {
        e->cs = false;
        ee93_reset(e);
        return;
    }
    e->cs = true;
    if (!rise)
        return;
    switch (e->state) {
    case EE_IDLE:
        if (di) { /* bit de inicio */
            e->state = EE_CMD;
            e->bits = 0;
            e->shift = 0;
        }
        break;
    case EE_CMD:
        e->shift = (e->shift << 1) | (di ? 1 : 0);
        if (++e->bits == 8) { /* 2 bits de operacao + 6 de endereco */
            unsigned op = (e->shift >> 6) & 3;
            e->addr = e->shift & 0x3f;
            if (op == 2) {
                e->state = EE_OUT;
                e->out = e->data[e->addr];
                e->outbits = 16;
                e->dout = false; /* bit fantasma */
            } else {
                e->state = EE_IDLE; /* escrita/apagamento: ignorados */
            }
        }
        break;
    case EE_OUT:
        if (!e->outbits) { /* leitura sequencial: proxima palavra */
            e->addr = (e->addr + 1) & 0x3f;
            e->out = e->data[e->addr];
            e->outbits = 16;
        }
        e->dout = (e->out >> (e->outbits - 1)) & 1;
        e->outbits--;
        break;
    }
}
