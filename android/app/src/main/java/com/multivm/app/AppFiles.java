package com.multivm.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import com.multivm.core.Architecture;
import com.multivm.core.DiskImages;

import java.io.File;
import java.io.IOException;

/** Ajudantes de arquivos e constantes comuns a MainActivity e WizardActivity. */
final class AppFiles {

    static final Architecture[] ARCHS = {Architecture.X86_64, Architecture.I386, Architecture.ARM64, Architecture.ARM};
    static final String[] ARCH_NAMES = {"x86-64 (PC)", "i386 (PC)", "ARM64 (virt)", "ARMv7 (virt)"};

    /** limites de memoria aceitos por VmConfig */
    static final int RAM_MIN = 4, RAM_MAX = 3072;

    /** abaixo disto o live do Void/Debian com desktop para em "Kernel panic ... deadlocked on memory" */
    static final int RAM_DESKTOP = 1024;

    private AppFiles() {}

    /** RAM sugerida para VMs novas: 1 GiB, ou 1,5 GiB se o aparelho tiver 6 GiB ou mais. */
    static int defaultRamMb(Context c) {
        try {
            android.app.ActivityManager am = (android.app.ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
            android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            if ((mi.totalMem >> 20) >= 5800) return 1536;
        } catch (Exception ignored) {
        }
        return RAM_DESKTOP;
    }

    /** Aviso de memoria pouca para sistemas com desktop, ou null. */
    static String lowRamWarning(int mb) {
        if (mb <= 0 || mb >= RAM_DESKTOP) return null;
        return "Pouca memória: Linux com desktop (Void, Debian, Ubuntu com Xfce, GNOME…) precisa de pelo menos "
                + RAM_DESKTOP + " MiB. Com menos, o boot pode parar em “Kernel panic … deadlocked on memory”. "
                + "Windows XP, ReactOS, DOS e Linux em modo texto funcionam com menos.";
    }

    static boolean isX86(Architecture a) {
        return a == Architecture.X86_64 || a == Architecture.I386;
    }

    /** Intent do seletor de arquivos (SAF); writable pede tambem permissao de escrita. */
    static Intent pickIntent(boolean writable) {
        Intent it = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        it.addCategory(Intent.CATEGORY_OPENABLE);
        it.setType("*/*");
        it.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                | (writable ? Intent.FLAG_GRANT_WRITE_URI_PERMISSION : 0));
        return it;
    }

    /** Guarda a permissao do arquivo escolhido e devolve a URI como texto. */
    @android.annotation.SuppressLint("WrongConstant") /* flags ja mascaradas para READ/WRITE */
    static String takePersistable(Context ctx, Intent data) {
        Uri uri = data.getData();
        int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            ctx.getContentResolver().takePersistableUriPermission(uri, flags);
        } catch (Exception ignored) {
            /* sem permissao persistente: vale so ate o app fechar */
        }
        return uri.toString();
    }

    static String displayName(Context ctx, String f) {
        if (f.startsWith("/")) return new File(f).getName();
        Uri uri = Uri.parse(f);
        try (Cursor c = ctx.getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE},
                null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String name = c.getString(0);
                long size = c.isNull(1) ? -1 : c.getLong(1);
                return size >= 0 ? name + "  (" + sizeStr(size) + ")" : name;
            }
        } catch (Exception ignored) {
        }
        String s = uri.getLastPathSegment();
        return s != null ? s : f;
    }

    static String sizeStr(long b) {
        if (b >= 1L << 30) return String.format(java.util.Locale.ROOT, "%.1f GiB", b / (double) (1L << 30));
        if (b >= 1L << 20) return String.format(java.util.Locale.ROOT, "%.1f MiB", b / (double) (1L << 20));
        return (b >> 10) + " KiB";
    }

    static File diskDir(Context ctx) {
        File base = ctx.getExternalFilesDir(null);
        if (base == null) base = ctx.getFilesDir();
        return new File(base, "disks");
    }

    /** Normaliza o nome de um disco novo: sem '/', com extensao .mvd se faltar. */
    static String diskFileName(String name) {
        String n = name.trim().replace('/', '_');
        if (!n.isEmpty() && !n.contains(".")) n += ".mvd";
        return n;
    }

    /** Liga o campo de memoria (MiB) a uma barra de 64 em 64 MiB; after roda a cada mudanca. */
    static void bindRam(EditText ram, SeekBar bar, Runnable after) {
        final int step = 64;
        bar.setMax(RAM_MAX / step - 1);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                if (fromUser) ram.setText(String.valueOf((p + 1) * step));
            }

            @Override
            public void onStartTrackingTouch(SeekBar s) {}

            @Override
            public void onStopTrackingTouch(SeekBar s) {}
        });
        ram.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}

            @Override
            public void onTextChanged(CharSequence s, int a, int b, int c) {}

            @Override
            public void afterTextChanged(Editable s) {
                try {
                    int mb = Integer.parseInt(s.toString().trim());
                    bar.setProgress(Math.max(0, Math.min(bar.getMax(), mb / step - 1)));
                } catch (NumberFormatException ignored) {
                }
                if (after != null) after.run();
            }
        });
    }

    /** Cria um disco MVD vazio na pasta de discos e devolve o caminho. */
    static String createMvd(Context ctx, String name, long gb) throws IOException {
        String n = diskFileName(name);
        if (n.isEmpty() || gb <= 0) throw new IllegalArgumentException("nome ou tamanho inválido");
        File dir = diskDir(ctx);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("não consegui criar " + dir);
        File f = new File(dir, n);
        if (f.exists()) throw new IOException(n + " já existe");
        DiskImages.createMvd(f.getAbsolutePath(), gb << 30);
        return f.getAbsolutePath();
    }
}
