package com.multivm.app;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import com.multivm.core.Architecture;
import com.multivm.core.DiskImage;
import com.multivm.core.VirtualMachine;
import com.multivm.core.VmConfig;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Monta a VmConfig a partir de uma VmSettings salva e inicia a VM (uma por vez). */
final class VmLauncher {

    interface Done {
        /** error == null: a VM iniciou e a tela dela foi aberta. Chamado na thread da UI. */
        void done(Throwable error);
    }

    private VmLauncher() {}

    /** Inicia a VM numa thread; ao terminar abre a VmActivity. */
    static void start(Activity a, VmSettings s, Done done) {
        new Thread(() -> {
            List<ParcelFileDescriptor> pfds = new ArrayList<>();
            try {
                boolean[] screen = new boolean[1];
                List<List<String>> media = new ArrayList<>();
                VmConfig cfg = buildConfig(a, s, pfds, screen, media);
                VirtualMachine vm = VirtualMachine.create(cfg);
                TerminalBuffer term = new TerminalBuffer();
                vm.addSerialListener(term::feed);
                VmHolder.set(vm, term, screen[0], s.id, s.name);
                VmHolder.setMedia(media, AppFiles.isX86(AppFiles.ARCHS[Math.max(0, Math.min(AppFiles.ARCHS.length - 1, s.arch))])
                        && s.bios && s.sata);
                vm.start();
                String vncError = null;
                if (s.vnc) {
                    int port = VmSettings.vncTcpPort(s.vncPort);
                    if (port < 0) {
                        vncError = "porta " + s.vncPort + " inválida: use um número de tela (0 a 99, ex.: 1 = 5901) "
                                + "ou uma porta de 1024 a 65535";
                    } else {
                        try {
                            vm.startVnc(port, s.vncPassword.isEmpty() ? null : s.vncPassword, s.vncLan);
                        } catch (IOException e) {
                            vncError = e.getMessage();
                        }
                    }
                }
                VmHolder.vncError = vncError;
                final String warn = vncError;
                a.runOnUiThread(() -> {
                    /* opcao de segundo plano ligada: notificacao fixa enquanto a VM existir */
                    VmService.sync(a);
                    done.done(null);
                    if (warn != null)
                        android.widget.Toast.makeText(a, "VNC não iniciou: " + warn, android.widget.Toast.LENGTH_LONG).show();
                    a.startActivity(new Intent(a, VmActivity.class));
                });
            } catch (Throwable ex) {
                a.runOnUiThread(() -> done.done(ex));
            } finally {
                /* a VM usa duplicatas dos descritores */
                for (ParcelFileDescriptor p : pfds) {
                    try { p.close(); } catch (IOException ignored) { }
                }
            }
        }, "MultiVM-start").start();
    }

    /**
     * media recebe, por tipo (VmSettings.KIND_*), os arquivos de cada drive na ordem em que
     * a VM os cria (null = drive vazio), para a tela da VM saber o que esta em cada slot.
     */
    private static VmConfig buildConfig(Activity a, VmSettings s, List<ParcelFileDescriptor> pfds, boolean[] screen,
                                        List<List<String>> media) throws IOException {
        for (int k = 0; k < 3; k++) media.add(new ArrayList<>());
        Architecture arch = AppFiles.ARCHS[Math.max(0, Math.min(AppFiles.ARCHS.length - 1, s.arch))];
        int mb;
        try {
            mb = Integer.parseInt(s.ram.trim());
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("memória inválida");
        }
        VmConfig.Builder b = VmConfig.builder(arch).ramMb(mb);
        if (arch == Architecture.X86_64 || arch == Architecture.I386)
            b.cpus(Math.max(1, Math.min(VmSettings.CPU_MAX, s.cpus)));
        boolean x86 = AppFiles.isX86(arch);
        boolean bios = x86 && s.bios;
        boolean useSata = bios && s.sata;
        List<VmSettings.Media> disks = s.disks();
        List<VmSettings.Media> cds = s.cdroms();
        List<VmSettings.Media> fds = x86 ? s.floppies() : new ArrayList<>();
        if (bios) {
            File dir = installBios(a);
            if (s.uefi) {
                if (arch != Architecture.X86_64)
                    throw new IllegalArgumentException("UEFI só está disponível para x86-64");
                b.firmware(new File(dir, "OVMF_CODE.fd").getAbsolutePath());
                b.uefiVars(uefiVars(a, s, dir).getAbsolutePath());
            } else {
                b.firmware(new File(dir, "bios-256k.bin").getAbsolutePath());
                b.vgaBios(new File(dir, "vgabios-stdvga.bin").getAbsolutePath());
            }
            if (s.bootOrder != null && !s.bootOrder.isEmpty()) b.bootOrder(s.bootOrder);
            if (disks.isEmpty() && cds.isEmpty() && fds.isEmpty())
                throw new IllegalArgumentException("adicione uma ISO, um disco ou um disquete para dar boot");
            screen[0] = true;
        } else {
            if (s.kernel == null) throw new IllegalArgumentException("escolha um kernel");
            b.kernel(copyToCache(a, s.kernel, "kernel"));
            if (s.initrd != null) b.initrd(copyToCache(a, s.initrd, "initrd"));
            String cl = s.cmdline == null ? "" : s.cmdline.trim();
            if (!cl.isEmpty()) b.cmdline(cl);
            if (!x86 && s.dtb != null) b.deviceTree(copyToCache(a, s.dtb, "dtb"));
            long addr = VmSettings.parseAddress(s.rawAddr);
            if (addr < 0) throw new IllegalArgumentException("endereço de carga inválido: " + s.rawAddr);
            if (addr > 0) b.rawLoadAddress(addr);
            if (s.fb) {
                String[] wh = s.fbSize.trim().toLowerCase(Locale.ROOT).split("x");
                try {
                    b.framebuffer(Integer.parseInt(wh[0].trim()), Integer.parseInt(wh[1].trim()));
                } catch (Exception e) {
                    throw new IllegalArgumentException("framebuffer inválido (use LARGURAxALTURA)");
                }
                screen[0] = true;
            }
        }
        /* ordem: discos, CDs, disquetes (o primeiro disco fica como mestre primario no IDE) */
        boolean useVirtio = bios && !useSata && s.virtio;
        DiskImage.Type hd = useSata ? DiskImage.Type.SATA : useVirtio ? DiskImage.Type.VIRTIO
                : bios ? DiskImage.Type.IDE : DiskImage.Type.AUTO;
        DiskImage.Type cd = useSata ? DiskImage.Type.SATA_CDROM : x86 ? DiskImage.Type.CDROM : DiskImage.Type.AUTO;
        int nHd = 0, nCd = 0, nFd = 0;
        for (VmSettings.Media m : disks)
            if (m.uri != null) {
                b.addDisk(openDisk(a, m.uri, m.ro, hd, pfds).withName(AppFiles.displayName(a, m.uri)));
                media.get(VmSettings.KIND_DISK).add(m.uri);
                nHd++;
            }
        for (VmSettings.Media m : cds)
            if (m.uri != null) {
                b.addDisk(openDisk(a, m.uri, true, cd, pfds).withName(AppFiles.displayName(a, m.uri)));
                media.get(VmSettings.KIND_CD).add(m.uri);
                nCd++;
            }
        for (VmSettings.Media m : fds)
            if (m.uri != null) {
                b.addDisk(openDisk(a, m.uri, m.ro, DiskImage.Type.FLOPPY, pfds).withName(AppFiles.displayName(a, m.uri)));
                media.get(VmSettings.KIND_FLOPPY).add(m.uri);
                nFd++;
            }
        /* com BIOS sempre ha um drive de CD e o A:, mesmo vazios, para trocar a midia com a VM ligada */
        if (bios && nCd == 0 && (useSata || useVirtio || nHd < 4)) {
            b.addDisk(DiskImage.emptyCdrom(cd));
            media.get(VmSettings.KIND_CD).add(null);
        }
        if (bios && nFd == 0) {
            b.addDisk(DiskImage.emptyFloppy());
            media.get(VmSettings.KIND_FLOPPY).add(null);
        }

        /* relogio */
        if (s.rtc == VmSettings.RTC_LOCAL) {
            long now = System.currentTimeMillis();
            b.rtcBase((now + java.util.TimeZone.getDefault().getOffset(now)) / 1000);
        } else if (s.rtc == VmSettings.RTC_FIXED) {
            long t = VmSettings.parseRtcDate(s.rtcDate);
            if (t < 0) throw new IllegalArgumentException("data do relógio inválida (use AAAA-MM-DD HH:MM): " + s.rtcDate);
            b.rtcBase(t);
        }

        /* rede NAT e redirecionamentos de porta (a maquina ARM so tem virtio: o resto vira automatica) */
        int nicIdx = contains(VmSettings.nicChoices(x86), s.nic) ? s.nic : 0;
        int sndIdx = contains(VmSettings.soundChoices(x86), s.sound) ? s.sound : 0;
        VmConfig.NicModel nic = VmSettings.NIC_MODELS[nicIdx];
        if (nic != VmConfig.NicModel.NONE) {
            b.network(nic, null).dns(hostDns(a));
            for (String f : s.forwards) {
                VmConfig.PortForward fw = VmSettings.parseForward(f);
                if (fw != null) b.addPortForward(fw);
            }
        }
        /* som (o microfone so com a permissao concedida) */
        VmConfig.SoundModel snd = VmSettings.SOUND_MODELS[sndIdx];
        boolean mic = s.mic && a.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                == android.content.pm.PackageManager.PERMISSION_GRANTED;
        b.audio(snd, mic);
        return b.build();
    }

    private static boolean contains(int[] a, int v) {
        for (int x : a) if (x == v) return true;
        return false;
    }

    /**
     * Antes de iniciar: se um CD de boot e um live que carrega o sistema inteiro na RAM (initrd enorme, ex.:
     * Fatdog64) e a memoria nao basta, pergunta se aumenta. go roda na thread da UI quando pode iniciar
     * (s.ram ja ajustada e salva se o usuario aceitou); cancel, se ele desistir.
     */
    /**
     * Antes de ligar: confere se as ISOs dao boot pelo firmware escolhido (BIOS x UEFI) e depois se a RAM
     * basta para o live CD (checkRamOnly). Oferece trocar o firmware quando so o outro serve.
     */
    static void checkRam(Activity a, VmSettings s, Runnable go, Runnable cancel) {
        Architecture arch = AppFiles.ARCHS[Math.max(0, Math.min(AppFiles.ARCHS.length - 1, s.arch))];
        List<String> isos = new ArrayList<>();
        if (AppFiles.isX86(arch) && s.bios)
            for (VmSettings.Media m : s.cdroms()) if (m.uri != null) isos.add(m.uri);
        if (isos.isEmpty()) {
            checkRamOnly(a, s, go, cancel);
            return;
        }
        new Thread(() -> {
            boolean anyBios = false, anyEfi = false, efiOnly = false, biosOnly = false;
            for (String f : isos) {
                int p = IsoInfo.bootPlatforms(a, f);
                anyBios |= (p & IsoInfo.BOOT_BIOS) != 0;
                anyEfi |= (p & IsoInfo.BOOT_EFI) != 0;
                efiOnly |= p == IsoInfo.BOOT_EFI;
                biosOnly |= p == IsoInfo.BOOT_BIOS;
            }
            final boolean wantUefi = !s.uefi && efiOnly && !anyBios;
            final boolean wantBios = s.uefi && biosOnly && !anyEfi;
            a.runOnUiThread(() -> {
                if (a.isFinishing()) {
                    cancel.run();
                    return;
                }
                if (!wantUefi && !wantBios) {
                    checkRamOnly(a, s, go, cancel);
                    return;
                }
                android.app.AlertDialog.Builder d = new android.app.AlertDialog.Builder(a)
                        .setNegativeButton("Iniciar assim mesmo", (x, w) -> checkRamOnly(a, s, go, cancel))
                        .setOnCancelListener(x -> cancel.run());
                if (wantUefi && arch != Architecture.X86_64) {
                    d.setTitle("Este CD só dá boot por UEFI")
                            .setMessage("A ISO só tem boot EFI, e o UEFI do MultiVM só existe para x86-64. Com BIOS ela "
                                    + "vai parar em “Could not read from CDROM (code 0009)”. Mude a arquitetura da VM para x86-64.");
                } else if (wantUefi) {
                    d.setTitle("Este CD só dá boot por UEFI")
                            .setMessage("A ISO só tem boot EFI. Com BIOS ela vai parar em “Could not read from CDROM "
                                    + "(code 0009)”.")
                            .setPositiveButton("Usar UEFI", (x, w) -> {
                                s.uefi = true;
                                s.save(a);
                                checkRamOnly(a, s, go, cancel);
                            });
                } else {
                    d.setTitle("Este CD só dá boot por BIOS")
                            .setMessage("A ISO não tem boot EFI: o firmware UEFI não vai achar o que iniciar.")
                            .setPositiveButton("Usar BIOS", (x, w) -> {
                                s.uefi = false;
                                s.save(a);
                                checkRamOnly(a, s, go, cancel);
                            });
                }
                d.show();
            });
        }, "MultiVM-iso-boot").start();
    }

    private static void checkRamOnly(Activity a, VmSettings s, Runnable go, Runnable cancel) {
        List<String> isos = new ArrayList<>();
        boolean x86 = AppFiles.isX86(AppFiles.ARCHS[Math.max(0, Math.min(AppFiles.ARCHS.length - 1, s.arch))]);
        if (x86 && s.bios)
            for (VmSettings.Media m : s.cdroms()) if (m.uri != null) isos.add(m.uri);
        int mb;
        try {
            mb = Integer.parseInt(s.ram.trim());
        } catch (NumberFormatException e) {
            mb = -1;
        }
        if (isos.isEmpty() || mb <= 0) {
            go.run();
            return;
        }
        final int cur = mb;
        new Thread(() -> {
            long initrd = 0;
            for (String f : isos) initrd = Math.max(initrd, IsoInfo.largestInitrd(a, f));
            final int need = IsoInfo.minRamMb(initrd);
            final long initrdMb = initrd >> 20;
            a.runOnUiThread(() -> {
                if (a.isFinishing()) {
                    cancel.run();
                    return;
                }
                if (need <= cur) {
                    go.run();
                    return;
                }
                String msg = "Este CD carrega o sistema inteiro na memória (initrd de " + initrdMb + " MiB). Com "
                        + cur + " MiB o boot deve parar numa tela preta ou em “Kernel panic … deadlocked on memory”.";
                android.app.AlertDialog.Builder d = new android.app.AlertDialog.Builder(a)
                        .setTitle("Memória pouca para este CD")
                        .setNegativeButton("Iniciar assim mesmo", (x, w) -> go.run())
                        .setOnCancelListener(x -> cancel.run());
                if (need <= AppFiles.RAM_MAX) {
                    d.setMessage(msg + "\n\nRecomendado: pelo menos " + need + " MiB.")
                            .setPositiveButton("Usar " + need + " MiB", (x, w) -> {
                                s.ram = String.valueOf(need);
                                s.save(a);
                                go.run();
                            });
                } else {
                    d.setMessage(msg + "\n\nEle precisaria de uns " + need + " MiB, acima do máximo do MultiVM ("
                            + AppFiles.RAM_MAX + " MiB).").setPositiveButton("Cancelar", (x, w) -> cancel.run());
                }
                d.show();
            });
        }, "MultiVM-iso").start();
    }

    /** O microfone foi pedido mas a permissao ainda nao foi dada. */
    static boolean needsMicPermission(Activity a, VmSettings s) {
        return s.mic && s.sound != VmSettings.SOUND_NONE
                && a.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                != android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    /** Primeiro servidor DNS IPv4 da rede ativa do aparelho (null: o nucleo usa 8.8.8.8). */
    static String hostDns(Activity a) {
        try {
            android.net.ConnectivityManager cm = (android.net.ConnectivityManager) a.getSystemService(
                    android.content.Context.CONNECTIVITY_SERVICE);
            android.net.Network net = cm.getActiveNetwork();
            android.net.LinkProperties lp = net != null ? cm.getLinkProperties(net) : null;
            if (lp != null)
                for (java.net.InetAddress d : lp.getDnsServers())
                    if (d instanceof java.net.Inet4Address) return d.getHostAddress();
        } catch (Exception ignored) {
        }
        return null;
    }

    /** Endereco IPv4 do aparelho na rede local (Wi-Fi), para mostrar onde o VNC atende. */
    static String lanAddress() {
        try {
            for (java.net.NetworkInterface ni : java.util.Collections.list(java.net.NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (java.net.InetAddress ad : java.util.Collections.list(ni.getInetAddresses()))
                    if (ad instanceof java.net.Inet4Address && ad.isSiteLocalAddress()) return ad.getHostAddress();
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    private static DiskImage openDisk(Activity a, String f, boolean ro, DiskImage.Type type,
                                      List<ParcelFileDescriptor> pfds) throws IOException {
        if (f.startsWith("/")) return DiskImage.fromPath(f, ro, type);
        ParcelFileDescriptor pfd = a.getContentResolver().openFileDescriptor(Uri.parse(f), ro ? "r" : "rw");
        if (pfd == null) throw new IOException("não consegui abrir " + AppFiles.displayName(a, f));
        pfds.add(pfd);
        return DiskImage.fromFileDescriptor(pfd, ro, type);
    }

    /** O nucleo le kernel/initrd por caminho: copia para o cache do app. */
    private static String copyToCache(Activity a, String f, String name) throws IOException {
        if (f.startsWith("/")) return f;
        File out = new File(a.getCacheDir(), name);
        try (InputStream in = a.getContentResolver().openInputStream(Uri.parse(f));
             OutputStream os = new FileOutputStream(out)) {
            if (in == null) throw new IOException("não consegui abrir " + AppFiles.displayName(a, f));
            copy(in, os);
        }
        return out.getAbsolutePath();
    }

    /** Extrai a BIOS embutida (assets/bios) para o armazenamento do app. */
    /** Variaveis UEFI da VM: na primeira vez, copia o modelo vazio (OVMF_VARS.fd). */
    private static File uefiVars(Activity a, VmSettings s, File biosDir) throws IOException {
        if (s.id == null) throw new IOException("salve a VM antes de iniciar com UEFI");
        File f = VmSettings.uefiVarsFile(a, s.id);
        if (f.isFile() && f.length() > 0) return f;
        File parent = f.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("não consegui criar " + parent);
        File tmp = new File(parent, f.getName() + ".tmp");
        try (InputStream in = new java.io.FileInputStream(new File(biosDir, "OVMF_VARS.fd"));
             OutputStream os = new FileOutputStream(tmp)) {
            copy(in, os);
        }
        if (!tmp.renameTo(f)) throw new IOException("não consegui criar " + f);
        return f;
    }

    private static File installBios(Activity a) throws IOException {
        File dir = new File(a.getFilesDir(), "bios");
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("não consegui criar " + dir);
        long stamp = new File(a.getApplicationInfo().sourceDir).lastModified();
        File mark = new File(dir, ".stamp-" + stamp);
        if (mark.exists()) return dir;
        String[] names = a.getAssets().list("bios");
        if (names != null) {
            for (String n : names) {
                try (InputStream in = a.getAssets().open("bios/" + n);
                     OutputStream os = new FileOutputStream(new File(dir, n))) {
                    copy(in, os);
                }
            }
        }
        File[] old = dir.listFiles((d, n) -> n.startsWith(".stamp-"));
        if (old != null) for (File o : old) o.delete();
        mark.createNewFile();
        return dir;
    }

    private static void copy(InputStream in, OutputStream os) throws IOException {
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
    }
}
