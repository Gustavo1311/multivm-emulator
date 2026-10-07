package com.multivm.app;

/**
 * Texto da serial do convidado. Recebe bytes na thread da VM, decodifica UTF-8 e
 * descarta sequencias de escape ANSI (o terminal e so texto). Guarda as ultimas
 * {@link #MAX} letras; a UI consulta {@link #snapshot} periodicamente.
 */
final class TerminalBuffer {
    private static final int MAX = 64 * 1024;

    private final StringBuilder text = new StringBuilder();
    /** total de caracteres ja acrescentados (sobe sempre, mesmo com o corte do inicio) */
    private long total;
    /** muda quando o texto ja mostrado foi alterado (backspace, limpeza) */
    private int rewrites;

    private static final int NORMAL = 0, ESC = 1, CSI = 2, OSC = 3, OSC_ESC = 4;
    private int esc = NORMAL;
    private int utfCp, utfLeft;

    synchronized void feed(byte[] data) {
        for (byte b : data) {
            int c = b & 0xff;
            if (utfLeft > 0) {
                if ((c & 0xc0) == 0x80) {
                    utfCp = (utfCp << 6) | (c & 0x3f);
                    if (--utfLeft == 0) put(utfCp);
                    continue;
                }
                utfLeft = 0;            /* sequencia invalida: recomeca */
            }
            if (c >= 0xc0 && c < 0xf8) {
                utfLeft = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
                utfCp = c & (0x3f >> utfLeft);
                continue;
            }
            put(c >= 0x80 ? '?' : c);
        }
    }

    private void put(int c) {
        switch (esc) {
            case ESC:
                esc = c == '[' ? CSI : c == ']' ? OSC : NORMAL;
                return;
            case CSI:
                if (c >= 0x40 && c <= 0x7e) {
                    esc = NORMAL;
                    if (c == 'J') clear();          /* limpar tela */
                }
                return;
            case OSC:
                if (c == 7) esc = NORMAL;
                else if (c == 0x1b) esc = OSC_ESC;
                return;
            case OSC_ESC:
                esc = NORMAL;
                return;
            default:
                break;
        }
        if (c == 0x1b) {
            esc = ESC;
        } else if (c == '\b') {
            int n = text.length();
            if (n > 0 && text.charAt(n - 1) != '\n') {
                text.setLength(n - 1);
                total--;
                rewrites++;
            }
        } else if (c == '\n' || c == '\t' || c >= 0x20) {
            if (c != 0x7f) {
                text.appendCodePoint(c);
                total++;
            }
        }
        /* '\r', BEL e outros controles sao ignorados */
        if (text.length() > MAX + MAX / 4) {
            text.delete(0, text.length() - MAX);
        }
    }

    synchronized void clear() {
        text.setLength(0);
        rewrites++;
    }

    /** Estado lido pela UI. */
    static final class Snapshot {
        long total;
        int rewrites = -1;
    }

    /**
     * Devolve o texto novo desde 'seen' (e atualiza 'seen'), ou o texto inteiro com
     * replace = true quando a UI precisa redesenhar tudo.
     */
    synchronized String snapshot(Snapshot seen, boolean[] replace) {
        long fresh = total - seen.total;
        boolean all = seen.rewrites != rewrites || fresh < 0 || fresh > text.length();
        seen.total = total;
        seen.rewrites = rewrites;
        replace[0] = all;
        if (all) return text.toString();
        if (fresh == 0) return null;
        return text.substring(text.length() - (int) fresh);
    }
}
