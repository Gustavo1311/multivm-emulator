package com.multivm.core;

/** Configuracoes globais da biblioteca. */
public final class MultiVm {
    public enum LogLevel { ERROR, WARN, INFO, DEBUG }

    public interface LogListener {
        void onLog(LogLevel level, String message);
    }

    private MultiVm() {}

    /** Versao do nucleo nativo. */
    public static String version() {
        return NativeBridge.nativeVersion();
    }

    public static void setLogLevel(LogLevel level) {
        NativeBridge.nativeSetLogLevel(level.ordinal());
    }

    /**
     * Recebe as mensagens de log do nucleo (pode ser chamado de qualquer thread).
     * Sem listener, o log vai para o logcat com a tag "MultiVM".
     */
    public static void setLogListener(LogListener listener) {
        if (listener == null) {
            NativeBridge.nativeSetLogListener(null);
            return;
        }
        NativeBridge.nativeSetLogListener((level, msg) -> {
            LogLevel[] v = LogLevel.values();
            listener.onLog(v[Math.max(0, Math.min(level, v.length - 1))], msg);
        });
    }
}
