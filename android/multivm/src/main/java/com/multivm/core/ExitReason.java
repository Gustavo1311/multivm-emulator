package com.multivm.core;

/** Motivo pelo qual a execucao da VM terminou. */
public enum ExitReason {
    /** {@link VirtualMachine#stop()} foi chamado. */
    STOPPED,
    /** O sistema convidado desligou a maquina. */
    SHUTDOWN,
    /** Erro fatal na emulacao (detalhes no log). */
    ERROR;

    static ExitReason fromNative(int v) {
        switch (v) {
            case 1: return SHUTDOWN;
            case 2: return ERROR;
            default: return STOPPED;
        }
    }
}
