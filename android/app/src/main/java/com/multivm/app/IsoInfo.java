package com.multivm.app;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.util.Locale;

/**
 * Le o diretorio de uma ISO 9660 para descobrir quanta RAM o live CD precisa. Distribuicoes como Fatdog64 e
 * Puppy poem o sistema inteiro no initrd (centenas de MiB), que o kernel descompacta na RAM: com memoria
 * pouca a tela fica preta ou o boot para em "Kernel panic ... deadlocked on memory".
 */
final class IsoInfo {

    private static final int SECTOR = 2048;

    private IsoInfo() {}

    /** Tamanho em bytes do maior arquivo initrd* na raiz ou em /boot da ISO, ou 0 se nao achar. */
    static long largestInitrd(Context c, String f) {
        try (ParcelFileDescriptor pfd = open(c, f)) {
            if (pfd == null) return 0;
            try (FileChannel ch = new java.io.FileInputStream(pfd.getFileDescriptor()).getChannel()) {
                ByteBuffer pvd = read(ch, 16L * SECTOR, SECTOR);
                if (pvd.get(0) != 1 || pvd.get(1) != 'C' || pvd.get(2) != 'D') return 0;
                long rootLba = pvd.getInt(156 + 2) & 0xffffffffL;
                int rootLen = pvd.getInt(156 + 10);
                long best = 0;
                long[] boot = new long[2];
                best = Math.max(best, scan(ch, rootLba, rootLen, boot));
                if (boot[0] != 0) best = Math.max(best, scan(ch, boot[0], (int) boot[1], null));
                return best;
            }
        } catch (IOException | RuntimeException e) {
            return 0;
        }
    }

    /** Boot El Torito: entrada para PC com BIOS (plataforma 0) e/ou para UEFI (0xEF). */
    static final int BOOT_BIOS = 1, BOOT_EFI = 2;

    /** Plataformas do catalogo El Torito (BOOT_BIOS | BOOT_EFI); 0 se a ISO nao da boot ou nao deu para ler. */
    static int bootPlatforms(Context c, String f) {
        try (ParcelFileDescriptor pfd = open(c, f)) {
            if (pfd == null) return 0;
            try (FileChannel ch = new java.io.FileInputStream(pfd.getFileDescriptor()).getChannel()) {
                ByteBuffer br = read(ch, 17L * SECTOR, SECTOR);
                byte[] id = new byte[23];
                br.position(7);
                br.get(id);
                if (br.get(0) != 0 || !new String(id, java.nio.charset.StandardCharsets.US_ASCII).equals("EL TORITO SPECIFICATION"))
                    return 0;
                long cat = br.getInt(0x47) & 0xffffffffL;
                ByteBuffer d = read(ch, cat * SECTOR, SECTOR);
                if (d.get(0) != 1 || (d.get(30) & 0xff) != 0x55 || (d.get(31) & 0xff) != 0xaa) return 0;
                int r = 0;
                int plat = d.get(1) & 0xff;
                if ((d.get(32) & 0xff) == 0x88) r |= plat == 0xef ? BOOT_EFI : plat == 0 ? BOOT_BIOS : 0;
                /* secoes seguintes: cabecalho 0x90 (mais secoes) / 0x91 (ultima) + n entradas */
                for (int p = 64; p + 32 <= SECTOR; ) {
                    int h = d.get(p) & 0xff;
                    if (h != 0x90 && h != 0x91) break;
                    int sp = d.get(p + 1) & 0xff, n = d.getShort(p + 2) & 0xffff;
                    p += 32;
                    for (int i = 0; i < n && p + 32 <= SECTOR; i++, p += 32) {
                        if ((d.get(p) & 0xff) == 0x44) { i--; continue; } /* extensao de entrada */
                        if ((d.get(p) & 0xff) == 0x88) r |= sp == 0xef ? BOOT_EFI : sp == 0 ? BOOT_BIOS : 0;
                    }
                    if (h == 0x91) break;
                }
                return r;
            }
        } catch (IOException | RuntimeException e) {
            return 0;
        }
    }

    /** Percorre um diretorio; devolve o maior initrd* e, se pedido, onde fica o subdiretorio BOOT. */
    private static long scan(FileChannel ch, long lba, int len, long[] bootDir) throws IOException {
        if (len <= 0 || len > (1 << 20)) return 0;
        ByteBuffer d = read(ch, lba * SECTOR, len);
        long best = 0;
        int p = 0;
        while (p < len) {
            int rl = d.get(p) & 0xff;
            if (rl == 0) { /* registros nao cruzam setores: pula para o proximo */
                p = (p / SECTOR + 1) * SECTOR;
                continue;
            }
            if (p + 33 > len || rl < 34) break;
            long ext = d.getInt(p + 2) & 0xffffffffL;
            long size = d.getInt(p + 10) & 0xffffffffL;
            boolean dir = (d.get(p + 25) & 2) != 0;
            int nl = d.get(p + 32) & 0xff;
            if (p + 33 + nl > len) break;
            byte[] nb = new byte[nl];
            for (int i = 0; i < nl; i++) nb[i] = d.get(p + 33 + i);
            String name = new String(nb, java.nio.charset.StandardCharsets.ISO_8859_1).toUpperCase(Locale.ROOT);
            if (dir) {
                if (bootDir != null && name.equals("BOOT")) {
                    bootDir[0] = ext;
                    bootDir[1] = size;
                }
            } else if (name.startsWith("INITRD")) {
                best = Math.max(best, size);
            }
            p += rl;
        }
        return best;
    }

    private static ByteBuffer read(FileChannel ch, long pos, int len) throws IOException {
        ByteBuffer b = ByteBuffer.allocate(len).order(ByteOrder.LITTLE_ENDIAN);
        while (b.hasRemaining()) {
            int n = ch.read(b, pos + b.position());
            if (n < 0) throw new IOException("ISO curta");
        }
        b.flip();
        return b;
    }

    private static ParcelFileDescriptor open(Context c, String f) throws IOException {
        if (f.startsWith("/")) return ParcelFileDescriptor.open(new File(f), ParcelFileDescriptor.MODE_READ_ONLY);
        return c.getContentResolver().openFileDescriptor(Uri.parse(f), "r");
    }

    /**
     * RAM minima (MiB, multiplo de 128) para o live CD com o maior initrd das ISOs, ou 0 se o initrd for
     * pequeno. Medido no Fatdog64 9.0.4 (initrd de 605 MiB): 1280 MiB trava, 1536 MiB chega ao desktop.
     */
    static int minRamMb(long initrdBytes) {
        long mib = initrdBytes >> 20;
        if (mib < 64) return 0;
        long need = mib * 2 + 256;
        return (int) ((need + 127) / 128 * 128);
    }
}
