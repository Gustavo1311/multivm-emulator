package com.multivm.app;

import com.multivm.core.VirtualMachine;

import java.util.ArrayList;
import java.util.List;

/** A VM em execucao no processo (uma por vez); sobrevive a recriacao das activities. */
final class VmHolder {
    static VirtualMachine vm;
    static TerminalBuffer terminal;
    /** true quando a maquina tem tela (BIOS/VGA ou framebuffer) */
    static boolean hasScreen;
    /** id e nome (VmSettings) da VM em execucao */
    static String vmId, vmName;
    /** por que o VNC desta VM nao iniciou (null = sem erro) */
    static volatile String vncError;
    /** arquivo em cada drive, por tipo (VmSettings.KIND_*) na ordem dos slots; null = vazio */
    static List<List<String>> media = new ArrayList<>();
    /** a VM usa o controlador SATA (aceita conectar discos com ela ligada) */
    static boolean sata;

    private VmHolder() {}

    static synchronized void setMedia(List<List<String>> m, boolean useSata) {
        media = m;
        sata = useSata;
    }

    static synchronized void set(VirtualMachine v, TerminalBuffer t, boolean screen, String id, String name) {
        vm = v;
        terminal = t;
        hasScreen = screen;
        vmId = id;
        vmName = name;
    }

    /** true se a VM com esse id e a que esta rodando */
    static synchronized boolean isRunning(String id) {
        return vm != null && id != null && id.equals(vmId);
    }

    /** Fecha a VM atual (bloqueia ate a thread da VM terminar). */
    static void closeCurrent() {
        VirtualMachine v;
        synchronized (VmHolder.class) {
            v = vm;
            vm = null;
            vmId = null;
        }
        if (v != null) v.close();
    }
}
