package com.multivm.core;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Configuracao imutavel de uma maquina virtual. Use {@link Builder}. */
public final class VmConfig {
    /** Total de midias (igual a MVM_MAX_DISKS no nucleo). */
    public static final int MAX_DISKS = 10;
    /** Limites por tipo: discos rigidos, CD/DVD e disquetes. */
    public static final int MAX_HARD_DISKS = 4, MAX_CDROMS = 4, MAX_FLOPPIES = 2;

    final Architecture arch;
    final int ramMb;
    final String kernel, initrd, cmdline, dtb, firmware, vgaBios, bootOrder;
    final List<DiskImage> disks;
    final int fbWidth, fbHeight;
    final long rawLoadAddress;
    final NicModel nic;
    final String mac, dns;
    final List<PortForward> forwards;
    final SoundModel sound;
    final boolean mic;

    /** Placa de rede (rede NAT em modo usuario: o convidado fica em 10.0.2.15). */
    public enum NicModel {
        NONE(0),
        /** PC com BIOS: RTL8139; boot direto e ARM: virtio-net. */
        AUTO(1),
        /** Realtek 8139: Windows XP, ReactOS, Linux. */
        RTL8139(2),
        /** Intel 82540EM (e1000): Windows 7/10/Server, Linux, ReactOS. */
        E1000(3),
        /** virtio-net: Linux (a mais rapida). */
        VIRTIO(4);

        final int nativeId;

        NicModel(int id) { nativeId = id; }
    }

    /** Placa de som. */
    public enum SoundModel {
        NONE(0),
        /** x86: AC'97; ARM: virtio-sound. */
        AUTO(1),
        /** Intel AC'97: Windows XP, ReactOS, Linux. */
        AC97(2),
        /** Intel HD Audio: Windows Vista/7/10/Server, Linux. */
        HDA(3),
        /** virtio-sound: Linux. */
        VIRTIO(4);

        final int nativeId;

        SoundModel(int id) { nativeId = id; }
    }

    /** Porta do aparelho repassada a uma porta do convidado. */
    public static final class PortForward {
        public final boolean udp, lan;
        public final int hostPort, guestPort;

        /** lan = true aceita conexoes da rede local; false, so deste aparelho. */
        public PortForward(boolean udp, int hostPort, int guestPort, boolean lan) {
            if (hostPort < 1 || hostPort > 65535 || guestPort < 1 || guestPort > 65535)
                throw new IllegalArgumentException("porta invalida");
            this.udp = udp;
            this.hostPort = hostPort;
            this.guestPort = guestPort;
            this.lan = lan;
        }
    }

    private VmConfig(Builder b) {
        arch = b.arch;
        ramMb = b.ramMb;
        kernel = b.kernel;
        initrd = b.initrd;
        cmdline = b.cmdline;
        dtb = b.dtb;
        firmware = b.firmware;
        vgaBios = b.vgaBios;
        bootOrder = b.bootOrder;
        disks = Collections.unmodifiableList(new ArrayList<>(b.disks));
        fbWidth = b.fbWidth;
        fbHeight = b.fbHeight;
        rawLoadAddress = b.rawLoadAddress;
        nic = b.nic;
        mac = b.mac;
        dns = b.dns;
        forwards = Collections.unmodifiableList(new ArrayList<>(b.forwards));
        sound = b.sound;
        mic = b.mic;
    }

    public NicModel getNicModel() { return nic; }
    public SoundModel getSoundModel() { return sound; }
    public boolean hasMicrophone() { return mic && sound != SoundModel.NONE; }
    public List<PortForward> getPortForwards() { return forwards; }

    public Architecture getArchitecture() { return arch; }
    public int getRamMb() { return ramMb; }
    public String getKernel() { return kernel; }
    public String getInitrd() { return initrd; }
    public String getCmdline() { return cmdline; }
    public List<DiskImage> getDisks() { return disks; }
    public String getFirmware() { return firmware; }
    public String getBootOrder() { return bootOrder; }
    public boolean hasFramebuffer() { return fbWidth > 0 && fbHeight > 0; }

    public static Builder builder(Architecture arch) {
        return new Builder(arch);
    }

    public static final class Builder {
        private final Architecture arch;
        private int ramMb = 256;
        private String kernel, initrd, cmdline, dtb, firmware, vgaBios, bootOrder;
        private final List<DiskImage> disks = new ArrayList<>();
        private int fbWidth, fbHeight;
        private long rawLoadAddress;
        private NicModel nic = NicModel.NONE;
        private String mac, dns;
        private final List<PortForward> forwards = new ArrayList<>();
        private SoundModel sound = SoundModel.NONE;
        private boolean mic;

        /** Rede NAT (convidado em 10.0.2.15, gateway e host em 10.0.2.2, DNS em 10.0.2.3). mac pode ser null. */
        public Builder network(NicModel model, String mac) {
            this.nic = model;
            this.mac = mac;
            return this;
        }

        /** Servidor DNS (IPv4) do aparelho, repassado ao convidado em 10.0.2.3. */
        public Builder dns(String server) { this.dns = server; return this; }

        public Builder addPortForward(PortForward f) {
            if (forwards.size() >= 16) throw new IllegalStateException("no maximo 16 redirecionamentos");
            forwards.add(f);
            return this;
        }

        /** Placa de som; mic liga a entrada (precisa da permissao RECORD_AUDIO). */
        public Builder audio(SoundModel model, boolean mic) {
            this.sound = model;
            this.mic = mic;
            return this;
        }

        Builder(Architecture arch) {
            if (arch == null) throw new NullPointerException("arch");
            this.arch = arch;
        }

        /** Memoria do convidado em MiB (4..3072). */
        public Builder ramMb(int mb) { this.ramMb = mb; return this; }

        /** Kernel Linux (bzImage, zImage, Image, Image.gz, EFI zboot) ou binario ELF/raw. */
        public Builder kernel(String path) { this.kernel = path; return this; }

        public Builder initrd(String path) { this.initrd = path; return this; }

        /** Linha de comando do kernel. Se nula, usa "console=<serial da arquitetura>". */
        public Builder cmdline(String cmdline) { this.cmdline = cmdline; return this; }

        /** ARM: device tree externo (por padrao e gerado automaticamente). */
        public Builder deviceTree(String path) { this.dtb = path; return this; }

        /**
         * x86: imagem de BIOS (ex.: SeaBIOS bios-256k.bin) mapeada no vetor de reset.
         * Sem kernel, a maquina inicia pela BIOS e da boot pelos discos IDE/CD-ROM.
         */
        public Builder firmware(String path) { this.firmware = path; return this; }

        /**
         * x86 com BIOS: option ROM da placa de video (vgabios-stdvga.bin). Se nula,
         * procura vgabios-stdvga.bin no mesmo diretorio do firmware.
         */
        public Builder vgaBios(String path) { this.vgaBios = path; return this; }

        /**
         * x86 com BIOS: ordem de boot como no QEMU ("c" = disco, "d" = CD-ROM,
         * "a" = disquete, "n" = rede), ex.: "dc". Se nula, usa CD-ROM primeiro quando
         * houver um; o disquete fica por ultimo.
         */
        public Builder bootOrder(String order) { this.bootOrder = order; return this; }

        public Builder addDisk(DiskImage disk) {
            if (disks.size() >= MAX_DISKS) throw new IllegalStateException("no maximo " + MAX_DISKS + " midias");
            int same = 0;
            for (DiskImage d : disks) if (kind(d.type) == kind(disk.type)) same++;
            int max = kind(disk.type) == 2 ? MAX_FLOPPIES : kind(disk.type) == 1 ? MAX_CDROMS : MAX_HARD_DISKS;
            if (same >= max)
                throw new IllegalStateException("no maximo " + max + (kind(disk.type) == 2 ? " disquetes"
                        : kind(disk.type) == 1 ? " CD/DVD" : " discos rigidos"));
            disks.add(disk);
            return this;
        }

        /** Framebuffer linear XRGB8888 exposto ao Linux (simplefb / vesafb). */
        public Builder framebuffer(int width, int height) {
            this.fbWidth = width;
            this.fbHeight = height;
            return this;
        }

        /** Endereco de carga de binarios brutos (0 = padrao da maquina). */
        public Builder rawLoadAddress(long addr) { this.rawLoadAddress = addr; return this; }

        /** 0 = disco rigido, 1 = CD/DVD, 2 = disquete */
        private static int kind(DiskImage.Type t) {
            if (t == DiskImage.Type.CDROM || t == DiskImage.Type.SATA_CDROM) return 1;
            if (t == DiskImage.Type.FLOPPY) return 2;
            return 0;
        }

        public VmConfig build() {
            if (kernel == null && firmware == null)
                throw new IllegalStateException("informe um kernel ou um firmware");
            if (ramMb < 4 || ramMb > 3072)
                throw new IllegalStateException("memoria invalida: " + ramMb + " MiB");
            boolean x86 = arch == Architecture.X86_64 || arch == Architecture.I386;
            for (DiskImage d : disks)
                if (d.type == DiskImage.Type.FLOPPY && !x86)
                    throw new IllegalStateException("disquete so existe nas maquinas x86");
            if (bootOrder != null && !bootOrder.matches("[a-dn]*"))
                throw new IllegalStateException("ordem de boot invalida: " + bootOrder);
            if (cmdline == null && kernel != null)
                cmdline = "console=" + arch.linuxConsole();
            return new VmConfig(this);
        }
    }
}
