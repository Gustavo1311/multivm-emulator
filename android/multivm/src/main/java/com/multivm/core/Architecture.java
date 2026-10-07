package com.multivm.core;

/** Arquiteturas de CPU suportadas pelo emulador. */
public enum Architecture {
    I386(0, "i386"),
    X86_64(1, "x86_64"),
    ARM(2, "arm"),
    ARM64(3, "arm64");

    final int nativeId;
    private final String id;

    Architecture(int nativeId, String id) {
        this.nativeId = nativeId;
        this.id = id;
    }

    /** Nome curto (i386, x86_64, arm, arm64). */
    public String id() {
        return id;
    }

    /** Maquina emulada: "pc" para x86, "virt" para ARM. */
    public String machine() {
        return this == I386 || this == X86_64 ? "pc" : "virt";
    }

    /** Console serial que o kernel Linux deve usar (parametro console=). */
    public String linuxConsole() {
        return this == I386 || this == X86_64 ? "ttyS0" : "ttyAMA0";
    }

    public static Architecture fromId(String name) {
        switch (name.toLowerCase(java.util.Locale.ROOT)) {
            case "i386": case "x86": case "i686": return I386;
            case "x86_64": case "x86-64": case "amd64": return X86_64;
            case "arm": case "armv7": case "aarch32": return ARM;
            case "arm64": case "aarch64": return ARM64;
            default: throw new IllegalArgumentException("arquitetura desconhecida: " + name);
        }
    }
}
