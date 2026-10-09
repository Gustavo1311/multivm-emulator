package com.multivm.app;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.List;

/**
 * Configuracao de uma VM, salva nas preferencias "vm_<id>". A lista de VMs fica em
 * "vms" (ids na ordem de criacao). Cada lista de midias (discos, CD/DVD, disquetes)
 * fica em chaves numeradas: disk0..3 (+ disk0Ro..), cdrom0..3, fda0..1 (+ fda0Ro..).
 */
final class VmSettings {

    /** preferencias da versao com uma VM so (migradas para vm_1) */
    private static final String OLD_PREFS = "config";
    private static final String REGISTRY = "vms";

    static final int KIND_DISK = 0, KIND_CD = 1, KIND_FLOPPY = 2;
    static final int[] MAX = {4, 4, 2};
    private static final String[] KEY = {"disk", "cdrom", "fda"};

    /** Opcoes de ordem de boot (BIOS): c = disco, d = CD/DVD, a = disquete. */
    static final String[] BOOT_NAMES = {"Automático (CD primeiro, se houver)", "Disco rígido primeiro",
            "CD/DVD primeiro", "Disquete primeiro", "Somente disco rígido"};
    static final String[] BOOT_CODES = {"", "cda", "dca", "acd", "c"};

    /** Placas de rede e de som (mesma ordem de VmConfig.NicModel/SoundModel em NIC_MODELS/SOUND_MODELS). */
    static final String[] NIC_NAMES = {"Automática", "Realtek RTL8139 (Windows XP, ReactOS, Linux)",
            "Intel e1000 (Windows 7/10/11/Server, Linux)", "virtio-net (Linux, a mais rápida)", "Sem rede"};
    static final com.multivm.core.VmConfig.NicModel[] NIC_MODELS = {
            com.multivm.core.VmConfig.NicModel.AUTO, com.multivm.core.VmConfig.NicModel.RTL8139,
            com.multivm.core.VmConfig.NicModel.E1000, com.multivm.core.VmConfig.NicModel.VIRTIO,
            com.multivm.core.VmConfig.NicModel.NONE};
    static final String[] SOUND_NAMES = {"Automática", "Intel AC'97 (Windows XP, ReactOS, Linux)",
            "Intel HD Audio (Windows Vista/7/10/11/Server, Linux)", "Sem som", "virtio-sound (Linux)"};
    static final com.multivm.core.VmConfig.SoundModel[] SOUND_MODELS = {
            com.multivm.core.VmConfig.SoundModel.AUTO, com.multivm.core.VmConfig.SoundModel.AC97,
            com.multivm.core.VmConfig.SoundModel.HDA, com.multivm.core.VmConfig.SoundModel.NONE,
            com.multivm.core.VmConfig.SoundModel.VIRTIO};
    static final int NIC_NONE = 4, SOUND_NONE = 3, NIC_VIRTIO = 3, SOUND_VIRTIO = 4;

    /** Placas oferecidas por arquitetura (indices em NIC_NAMES/SOUND_NAMES): a maquina ARM so tem virtio. */
    static int[] nicChoices(boolean x86) {
        return x86 ? new int[]{0, 1, 2, 3, 4} : new int[]{0, NIC_VIRTIO, NIC_NONE};
    }

    static int[] soundChoices(boolean x86) {
        return x86 ? new int[]{0, 1, 2, 3} : new int[]{0, SOUND_VIRTIO, SOUND_NONE};
    }

    /** Relogio do convidado (RTC). */
    static final int RTC_UTC = 0, RTC_LOCAL = 1, RTC_FIXED = 2;
    static final String[] RTC_NAMES = {"UTC (Linux, padrão)", "Hora local do aparelho (Windows, DOS)",
            "Data fixa (começa na data escolhida)"};

    /** Uma midia: arquivo (caminho ou URI) ou um disco novo ainda por criar (assistente). */
    static final class Media {
        String uri;
        boolean ro;
        String newName; /* != null: disco MVD a criar com newGb GiB */
        long newGb;

        Media(String uri, boolean ro) {
            this.uri = uri;
            this.ro = ro;
        }

        static Media pending(String name, long gb) {
            Media m = new Media(null, false);
            m.newName = name;
            m.newGb = gb;
            return m;
        }

        boolean isPending() { return uri == null && newName != null; }

        /* serializacao simples para onSaveInstanceState */
        String encode() {
            return (ro ? "1" : "0") + "|" + (newName != null ? newName : "") + "|" + newGb + "|" + (uri != null ? uri : "");
        }

        static Media decode(String s) {
            String[] p = s.split("\\|", 4);
            Media m = new Media(p[3].isEmpty() ? null : p[3], p[0].equals("1"));
            if (!p[1].isEmpty()) {
                m.newName = p[1];
                m.newGb = Long.parseLong(p[2]);
            }
            return m;
        }
    }

    /** null = VM nova, ainda nao salva */
    String id;
    String name = "MultiVM";
    int arch;
    String ram = "1024";
    boolean bios = true;
    String cmdline = "";
    boolean fb;
    String fbSize = "800x600";
    String bootOrder = "";
    boolean sata;
    /** discos rigidos em virtio-blk com BIOS (Linux): mais rapido; CDs continuam no IDE */
    boolean virtio;
    String kernel, initrd;
    /** boot direto: device tree externo (ARM) e endereco de carga de binario bruto ("" = padrao) */
    String dtb;
    String rawAddr = "";
    int rtc;
    String rtcDate = "2000-01-01 00:00";
    /* som e rede: indices em NIC_NAMES e SOUND_NAMES */
    int nic, sound;
    boolean mic;
    /** redirecionamentos: "tcp:PORTA_HOST:PORTA_CONVIDADO:lan|local" */
    final List<String> forwards = new ArrayList<>();
    /* acesso remoto */
    boolean vnc, vncLan;
    int vncPort = 5900;
    String vncPassword = "";
    @SuppressWarnings("unchecked")
    final List<Media>[] media = new List[]{new ArrayList<Media>(), new ArrayList<Media>(), new ArrayList<Media>()};

    List<Media> disks() { return media[KIND_DISK]; }
    List<Media> cdroms() { return media[KIND_CD]; }
    List<Media> floppies() { return media[KIND_FLOPPY]; }

    private static SharedPreferences prefs(Context ctx, String id) {
        return ctx.getSharedPreferences("vm_" + id, Context.MODE_PRIVATE);
    }

    private static SharedPreferences registry(Context ctx) {
        migrate(ctx);
        return ctx.getSharedPreferences(REGISTRY, Context.MODE_PRIVATE);
    }

    /** Versao antiga (uma VM em "config"): vira a VM 1. */
    private static void migrate(Context ctx) {
        SharedPreferences reg = ctx.getSharedPreferences(REGISTRY, Context.MODE_PRIVATE);
        if (reg.contains("ids")) return;
        SharedPreferences old = ctx.getSharedPreferences(OLD_PREFS, Context.MODE_PRIVATE);
        SharedPreferences.Editor r = reg.edit().putString("ids", "").putInt("nextId", 1)
                .putBoolean("setupDone", old.getBoolean("setupDone", false) || old.contains("arch"));
        if (old.contains("arch")) {
            SharedPreferences.Editor e = prefs(ctx, "1").edit();
            for (java.util.Map.Entry<String, ?> en : old.getAll().entrySet()) {
                Object v = en.getValue();
                if (v instanceof String) e.putString(en.getKey(), (String) v);
                else if (v instanceof Boolean) e.putBoolean(en.getKey(), (Boolean) v);
                else if (v instanceof Integer) e.putInt(en.getKey(), (Integer) v);
                else if (v instanceof Long) e.putLong(en.getKey(), (Long) v);
                else if (v instanceof Float) e.putFloat(en.getKey(), (Float) v);
            }
            e.commit();
            r.putString("ids", "1").putInt("nextId", 2);
        }
        r.commit();
        old.edit().clear().commit();
    }

    static List<String> ids(Context ctx) {
        List<String> out = new ArrayList<>();
        for (String s : registry(ctx).getString("ids", "").split(","))
            if (!s.isEmpty()) out.add(s);
        return out;
    }

    static List<VmSettings> all(Context ctx) {
        List<VmSettings> out = new ArrayList<>();
        for (String id : ids(ctx)) out.add(load(ctx, id));
        return out;
    }

    /** Primeira execucao: nenhuma VM e o assistente ainda nao foi concluido/pulado. */
    static boolean isFirstRun(Context ctx) {
        return ids(ctx).isEmpty() && !registry(ctx).getBoolean("setupDone", false);
    }

    static void markSetupDone(Context ctx) {
        registry(ctx).edit().putBoolean("setupDone", true).apply();
    }

    /** Apaga a configuracao da VM (os arquivos de disco ficam). */
    static void delete(Context ctx, String id) {
        List<String> ids = ids(ctx);
        ids.remove(id);
        registry(ctx).edit().putString("ids", String.join(",", ids)).commit();
        prefs(ctx, id).edit().clear().commit();
    }

    /** null se a VM nao existe (mais). */
    static VmSettings find(Context ctx, String id) {
        return id != null && ids(ctx).contains(id) ? load(ctx, id) : null;
    }

    static VmSettings load(Context ctx, String id) {
        SharedPreferences p = prefs(ctx, id);
        VmSettings s = new VmSettings();
        s.id = id;
        s.name = p.getString("name", "MultiVM");
        s.arch = p.getInt("arch", 0);
        s.ram = p.getString("ram", "1024");
        s.bios = p.getBoolean("bios", true);
        s.cmdline = p.getString("cmdline", "");
        s.fb = p.getBoolean("fb", false);
        s.fbSize = p.getString("fbSize", "800x600");
        s.bootOrder = p.getString("bootOrder", "");
        s.sata = p.getBoolean("sata", false);
        s.virtio = p.getBoolean("virtio", false) && !s.sata;
        s.kernel = p.getString("kernel", null);
        s.initrd = p.getString("initrd", null);
        s.dtb = p.getString("dtb", null);
        s.rawAddr = p.getString("rawAddr", "");
        s.rtc = clamp(p.getInt("rtc", RTC_UTC), RTC_NAMES.length);
        s.rtcDate = p.getString("rtcDate", "2000-01-01 00:00");
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < MAX[k]; i++) {
                String u = p.getString(KEY[k] + i, null);
                if (u != null) s.media[k].add(new Media(u, p.getBoolean(KEY[k] + i + "Ro", false)));
            }
        /* versoes antigas: um disco ("disk"/"diskRo") e um CD ("cdrom") */
        if (s.disks().isEmpty() && p.getString("disk", null) != null)
            s.disks().add(new Media(p.getString("disk", null), p.getBoolean("diskRo", false)));
        if (s.cdroms().isEmpty() && p.getString("cdrom", null) != null)
            s.cdroms().add(new Media(p.getString("cdrom", null), true));
        s.nic = clamp(p.getInt("nic", 0), NIC_NAMES.length);
        s.sound = clamp(p.getInt("sound", 0), SOUND_NAMES.length);
        s.mic = p.getBoolean("mic", false);
        for (String f : p.getString("forwards", "").split(","))
            if (parseForward(f) != null) s.forwards.add(f);
        s.vnc = p.getBoolean("vnc", false);
        s.vncLan = p.getBoolean("vncLan", false);
        s.vncPort = p.getInt("vncPort", 5900);
        s.vncPassword = p.getString("vncPassword", "");
        return s;
    }

    private static int clamp(int v, int n) { return v < 0 || v >= n ? 0 : v; }

    /**
     * Valor do campo "tela ou porta" do VNC para a porta TCP: 0-99 e um numero de tela
     * como nos clientes VNC (1 = 5901); 1024-65535 e a porta; o resto (100-1023) e
     * invalido porque o Android nao deixa apps escutarem abaixo de 1024. -1 se invalido.
     */
    static int vncTcpPort(int v) {
        if (v >= 0 && v <= 99) return 5900 + v;
        if (v >= 1024 && v <= 65535) return v;
        return -1;
    }

    /** "tcp:2222:22:local" -> redirecionamento (null se invalido) */
    static com.multivm.core.VmConfig.PortForward parseForward(String f) {
        String[] p = f.split(":");
        if (p.length != 4) return null;
        try {
            return new com.multivm.core.VmConfig.PortForward(p[0].equals("udp"), Integer.parseInt(p[1]),
                    Integer.parseInt(p[2]), p[3].equals("lan"));
        } catch (IllegalArgumentException e) {
            return null;
        }
    }

    static String forwardText(String f) {
        com.multivm.core.VmConfig.PortForward fw = parseForward(f);
        if (fw == null) return f;
        return (fw.udp ? "UDP " : "TCP ") + fw.hostPort + " → " + fw.guestPort
                + (fw.lan ? "  (rede local)" : "  (só este aparelho)");
    }

    /** Grava a VM; uma VM nova ganha um id e entra no fim da lista. */
    void save(Context ctx) {
        if (id == null) {
            SharedPreferences reg = registry(ctx);
            int n = reg.getInt("nextId", 1);
            id = String.valueOf(n);
            List<String> ids = ids(ctx);
            ids.add(id);
            reg.edit().putString("ids", String.join(",", ids)).putInt("nextId", n + 1)
                    .putBoolean("setupDone", true).commit();
        }
        SharedPreferences.Editor e = prefs(ctx, id).edit()
                .putString("name", name)
                .putInt("arch", arch)
                .putString("ram", ram)
                .putBoolean("bios", bios)
                .putString("cmdline", cmdline)
                .putBoolean("fb", fb)
                .putString("fbSize", fbSize)
                .putString("bootOrder", bootOrder)
                .putBoolean("sata", sata)
                .putBoolean("virtio", virtio)
                .putString("kernel", kernel)
                .putString("initrd", initrd)
                .putString("dtb", dtb)
                .putString("rawAddr", rawAddr == null ? "" : rawAddr)
                .putInt("rtc", rtc)
                .putString("rtcDate", rtcDate)
                .putInt("nic", nic)
                .putInt("sound", sound)
                .putBoolean("mic", mic)
                .putString("forwards", String.join(",", forwards))
                .putBoolean("vnc", vnc)
                .putBoolean("vncLan", vncLan)
                .putInt("vncPort", vncPort)
                .putString("vncPassword", vncPassword)
                .remove("disk").remove("diskRo").remove("cdrom");
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < MAX[k]; i++) {
                Media m = i < media[k].size() ? media[k].get(i) : null;
                if (m != null && m.uri != null) {
                    e.putString(KEY[k] + i, m.uri).putBoolean(KEY[k] + i + "Ro", m.ro);
                } else {
                    e.remove(KEY[k] + i).remove(KEY[k] + i + "Ro");
                }
            }
        e.commit();
    }

    /** Resumo de uma linha: "x86-64 (PC) · 512 MiB · 2 discos · 1 ISO". */
    String summary() {
        StringBuilder sb = new StringBuilder();
        sb.append(AppFiles.ARCH_NAMES[Math.max(0, Math.min(AppFiles.ARCH_NAMES.length - 1, arch))]);
        sb.append(" · ").append(ram.trim()).append(" MiB");
        int d = disks().size(), c = cdroms().size(), f = floppies().size();
        if (d > 0) sb.append(" · ").append(d).append(d == 1 ? " disco" : " discos");
        if (c > 0) sb.append(" · ").append(c).append(c == 1 ? " ISO" : " ISOs");
        if (f > 0 && AppFiles.isX86(AppFiles.ARCHS[Math.max(0, Math.min(3, arch))]))
            sb.append(" · ").append(f).append(f == 1 ? " disquete" : " disquetes");
        if (!bios) sb.append(" · kernel Linux");
        if (sound != SOUND_NONE) sb.append(" · som");
        if (nic != NIC_NONE) sb.append(" · rede");
        if (vnc) sb.append(" · VNC");
        return sb.toString();
    }

    /** "0x40080000", "40080000h" ou decimal -> endereco; -1 se invalido, 0 se vazio. */
    static long parseAddress(String v) {
        String t = v == null ? "" : v.trim().toLowerCase(java.util.Locale.ROOT).replace("_", "");
        if (t.isEmpty()) return 0;
        try {
            if (t.startsWith("0x")) return Long.parseUnsignedLong(t.substring(2), 16);
            if (t.endsWith("h")) return Long.parseUnsignedLong(t.substring(0, t.length() - 1), 16);
            if (t.matches(".*[a-f].*")) return Long.parseUnsignedLong(t, 16);
            return Long.parseLong(t);
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    /** "AAAA-MM-DD HH:MM" (hora local do aparelho) -> segundos Unix; -1 se invalido. */
    static long parseRtcDate(String v) {
        java.text.SimpleDateFormat f = new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm", java.util.Locale.ROOT);
        f.setLenient(false);
        try {
            java.util.Date d = f.parse(v == null ? "" : v.trim());
            long sec = d.getTime() / 1000;
            return sec > 0 ? sec : -1;
        } catch (java.text.ParseException e) {
            return -1;
        }
    }

    static int bootIndex(String code) {
        for (int i = 0; i < BOOT_CODES.length; i++)
            if (BOOT_CODES[i].equals(code)) return i;
        return -1;
    }
}
