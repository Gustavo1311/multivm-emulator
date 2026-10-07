package com.multivm.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.multivm.core.DiskImage;
import com.multivm.core.MediaSlot;
import com.multivm.core.VirtualMachine;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Painel "Midia" da tela da VM: lista os drives (CD, disquete, discos) e permite inserir,
 * trocar e ejetar com a VM ligada. Disco rigido so troca a quente no SATA; no IDE a
 * mudanca fica salva para o proximo boot. Tudo tambem e gravado na VmSettings.
 */
final class MediaPanel {
    static final int REQ_PICK = 7301;
    private static final int MAX_DISKS = 4;
    /** slot que recebe o arquivo escolhido; ADD_SATA / ADD_NEXT_BOOT para disco novo */
    private static final int ADD_SATA = -1, ADD_NEXT_BOOT = -2;

    private final Activity act;
    private final VirtualMachine vm;
    private AlertDialog dialog;
    private LinearLayout list;
    private int pickTarget;
    private boolean busy;

    MediaPanel(Activity act, VirtualMachine vm) {
        this.act = act;
        this.vm = vm;
    }

    void show() {
        list = new LinearLayout(act);
        list.setOrientation(LinearLayout.VERTICAL);
        int p = dp(16);
        list.setPadding(p, dp(4), p, dp(8));
        ScrollView sv = new ScrollView(act);
        sv.addView(list);
        dialog = new AlertDialog.Builder(act)
                .setTitle("Mídias da VM")
                .setView(sv)
                .setPositiveButton("Fechar", null)
                .create();
        refresh();
        dialog.show();
    }

    /** Resultado do seletor de arquivos (repassado pela activity). */
    void onPicked(Intent data) {
        if (data == null || data.getData() == null) return;
        String uri = AppFiles.takePersistable(act, data);
        int target = pickTarget;
        if (target == ADD_NEXT_BOOT) {
            VmSettings s = settings();
            if (s == null) return;
            s.disks().add(new VmSettings.Media(uri, false));
            s.save(act);
            toast("Disco adicionado. Ele aparece no próximo boot (o IDE não aceita conectar com a VM ligada).");
            refresh();
            return;
        }
        run(target == ADD_SATA ? "Conectando o disco..." : "Inserindo...", () -> {
            List<MediaSlot> slots = vm.getMediaSlots();
            MediaSlot slot = target >= 0 && target < slots.size() ? slots.get(target) : null;
            boolean cd = slot != null && slot.kind == MediaSlot.Kind.CDROM;
            boolean ro = cd || (slot != null && slot.kind == MediaSlot.Kind.FLOPPY && slot.readOnly);
            try (ParcelFileDescriptor pfd = open(uri, ro)) {
                DiskImage img = (pfd != null ? DiskImage.fromFileDescriptor(pfd, ro) : DiskImage.fromPath(uri, ro))
                        .withName(AppFiles.displayName(act, uri));
                if (target == ADD_SATA) {
                    int n = vm.addHardDisk(img);
                    remember(n, uri);
                } else {
                    vm.insertMedia(target, img);
                    remember(target, uri);
                }
            }
            return null;
        });
    }

    /* ---- lista ---- */

    private void refresh() {
        if (list == null) return;
        list.removeAllViews();
        List<MediaSlot> slots;
        try {
            slots = vm.getMediaSlots();
        } catch (Exception e) {
            slots = new ArrayList<>();
        }
        int nCd = 0, nHd = 0;
        for (MediaSlot m : slots) {
            switch (m.kind) {
                case CDROM: nCd++; addCd(m, nCd); break;
                case FLOPPY: addFloppy(m); break;
                default: nHd++; addDisk(m, nHd); break;
            }
        }
        if (slots.isEmpty()) {
            TextView t = text("Esta máquina não tem drives que possam ser trocados com ela ligada.", 13, R.color.text2);
            list.addView(t);
        }
        if (VmHolder.sata || bios()) {
            Button add = new Button(act, null, 0, R.style.KeyButton);
            add.setText("+ Adicionar disco rígido");
            add.setCompoundDrawablesRelativeWithIntrinsicBounds(R.drawable.ic_add, 0, 0, 0);
            add.setCompoundDrawablePadding(dp(6));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(44));
            lp.topMargin = dp(10);
            list.addView(add, lp);
            add.setOnClickListener(v -> addDiskClicked());
        }
        if (busy) list.addView(text("aguarde...", 12, R.color.accent));
    }

    private void addCd(MediaSlot m, int n) {
        LinearLayout row = row(R.drawable.ic_disc, "CD " + n + (m.bus == MediaSlot.Bus.SATA ? " (SATA)" : ""),
                m.present ? m.name : "(vazio)");
        if (m.present) {
            action(row, "Trocar", () -> pick(m.slot));
            action(row, "Ejetar", () -> eject(m, false));
        } else {
            action(row, "Inserir ISO", () -> pick(m.slot));
        }
    }

    private void addFloppy(MediaSlot m) {
        LinearLayout row = row(R.drawable.ic_floppy, "Disquete " + (m.unit == 0 ? "A:" : "B:"),
                m.present ? m.name : "(vazio)");
        if (m.present) {
            action(row, "Trocar", () -> pick(m.slot));
            action(row, "Ejetar", () -> eject(m, false));
        } else {
            action(row, "Inserir", () -> pick(m.slot));
        }
    }

    private void addDisk(MediaSlot m, int n) {
        String bus = m.bus == MediaSlot.Bus.SATA ? "SATA" : m.bus == MediaSlot.Bus.IDE ? "IDE" : "virtio";
        LinearLayout row = row(R.drawable.ic_hdd, "Disco " + n + " (" + bus + ")",
                m.present ? m.name + (m.readOnly ? "  · somente leitura" : "") : "(desconectado)");
        if (m.changeable) {
            if (m.present) action(row, "Remover", () -> eject(m, true));
            else action(row, "Conectar", () -> pick(m.slot));
        } else {
            action(row, "Remover", () -> removeNextBoot(m, n));
        }
    }

    private LinearLayout row(int icon, String title, String sub) {
        LinearLayout row = new LinearLayout(act);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setBackgroundResource(R.drawable.item_bg);
        row.setPadding(dp(12), dp(8), dp(6), dp(8));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(6);
        list.addView(row, lp);

        ImageView iv = new ImageView(act);
        iv.setImageResource(icon);
        iv.setImageTintList(android.content.res.ColorStateList.valueOf(act.getColor(R.color.accent)));
        row.addView(iv, new LinearLayout.LayoutParams(dp(24), dp(24)));

        LinearLayout col = new LinearLayout(act);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(12), 0, dp(6), 0);
        col.addView(text(title, 14, R.color.text));
        TextView s = text(sub, 12, R.color.text2);
        s.setSingleLine(true);
        s.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        col.addView(s);
        row.addView(col, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        return row;
    }

    private void action(LinearLayout row, String label, Runnable r) {
        Button b = new Button(act, null, 0, R.style.KeyButton);
        b.setText(label);
        b.setTextColor(act.getColor(R.color.accent));
        b.setEnabled(!busy);
        b.setOnClickListener(v -> r.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(36));
        lp.setMarginStart(dp(4));
        row.addView(b, lp);
    }

    private TextView text(String s, int sp, int color) {
        TextView t = new TextView(act);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(act.getColor(color));
        return t;
    }

    /* ---- acoes ---- */

    private void pick(int target) {
        pickTarget = target;
        try {
            act.startActivityForResult(AppFiles.pickIntent(true), REQ_PICK);
        } catch (Exception e) {
            toast("Nenhum seletor de arquivos disponível");
        }
    }

    private void addDiskClicked() {
        VmSettings s = settings();
        int count = 0;
        if (!VmHolder.sata) {
            if (s != null) count = s.disks().size();
        } else if (!VmHolder.media.isEmpty()) {
            for (String u : VmHolder.media.get(VmSettings.KIND_DISK)) if (u != null) count++;
        }
        if (count >= MAX_DISKS) {
            toast("Limite de " + MAX_DISKS + " discos rígidos");
            return;
        }
        if (VmHolder.sata) {
            pick(ADD_SATA);
            return;
        }
        new AlertDialog.Builder(act)
                .setTitle("Disco no IDE")
                .setMessage("O controlador IDE não aceita conectar disco com a VM ligada. O disco será adicionado "
                        + "às configurações e aparece no próximo boot.\n\nPara conectar com a VM ligada, use o "
                        + "controlador SATA nas configurações da máquina.")
                .setPositiveButton("Escolher disco", (d, w) -> pick(ADD_NEXT_BOOT))
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void eject(MediaSlot m, boolean hdd) {
        Runnable go = () -> run(hdd ? "Desconectando..." : "Ejetando...", () -> {
            vm.ejectMedia(m.slot);
            remember(m.slot, null);
            return null;
        });
        if (!hdd) {
            go.run();
            return;
        }
        new AlertDialog.Builder(act)
                .setTitle("Remover o disco?")
                .setMessage("Desconectar um disco em uso pode corromper os dados. Desmonte o disco no sistema "
                        + "convidado antes (no Windows: \"Remover hardware com segurança\").")
                .setPositiveButton("Remover", (d, w) -> go.run())
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void removeNextBoot(MediaSlot m, int n) {
        new AlertDialog.Builder(act)
                .setTitle("Remover no próximo boot")
                .setMessage("O controlador " + (m.bus == MediaSlot.Bus.IDE ? "IDE" : "virtio") + " não aceita "
                        + "desconectar disco com a VM ligada. O disco \"" + m.name + "\" será retirado das "
                        + "configurações e some no próximo boot.")
                .setPositiveButton("Remover", (d, w) -> {
                    VmSettings s = settings();
                    List<String> hd = VmHolder.media.isEmpty() ? null : VmHolder.media.get(VmSettings.KIND_DISK);
                    String uri = hd != null && n - 1 < hd.size() ? hd.get(n - 1) : null;
                    if (s != null && uri != null) {
                        for (int i = 0; i < s.disks().size(); i++)
                            if (uri.equals(s.disks().get(i).uri)) {
                                s.disks().remove(i);
                                break;
                            }
                        s.save(act);
                        toast("O disco sai no próximo boot");
                    }
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private interface Job {
        Object call() throws Exception;
    }

    /** Executa fora da thread da UI (a VM aplica a troca na thread dela e responde). */
    private void run(String msg, Job job) {
        busy = true;
        refresh();
        toast(msg);
        new Thread(() -> {
            String err = null;
            try {
                job.call();
            } catch (Exception e) {
                err = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final String fe = err;
            act.runOnUiThread(() -> {
                busy = false;
                if (fe != null) {
                    new AlertDialog.Builder(act).setTitle("Não foi possível").setMessage(fe)
                            .setPositiveButton("OK", null).show();
                }
                refresh();
            });
        }, "MultiVM-media").start();
    }

    private ParcelFileDescriptor open(String f, boolean ro) throws IOException {
        if (f.startsWith("/")) return null;
        ParcelFileDescriptor pfd;
        try {
            pfd = act.getContentResolver().openFileDescriptor(Uri.parse(f), ro ? "r" : "rw");
        } catch (SecurityException e) {
            /* sem permissao de escrita: abre so para leitura */
            if (ro) throw new IOException(e.getMessage());
            pfd = act.getContentResolver().openFileDescriptor(Uri.parse(f), "r");
        }
        if (pfd == null) throw new IOException("não consegui abrir " + AppFiles.displayName(act, f));
        return pfd;
    }

    /* ---- o que esta em cada drive, e gravacao nas configuracoes ---- */

    /** Anota o arquivo do slot (null = vazio) e grava a lista do tipo na VmSettings. */
    private void remember(int slot, String uri) {
        List<MediaSlot> slots = vm.getMediaSlots();
        if (slot < 0 || slot >= slots.size()) return;
        MediaSlot m = slots.get(slot);
        int kind = m.kind == MediaSlot.Kind.CDROM ? VmSettings.KIND_CD
                : m.kind == MediaSlot.Kind.FLOPPY ? VmSettings.KIND_FLOPPY : VmSettings.KIND_DISK;
        int idx = 0;
        for (int i = 0; i < slot; i++) if (slots.get(i).kind == m.kind) idx++;
        synchronized (VmHolder.class) {
            while (VmHolder.media.size() < 3) VmHolder.media.add(new ArrayList<>());
            List<String> l = VmHolder.media.get(kind);
            while (l.size() <= idx) l.add(null);
            l.set(idx, uri);
        }
        act.runOnUiThread(() -> persist(kind));
    }

    private void persist(int kind) {
        VmSettings s = settings();
        if (s == null) return;
        List<VmSettings.Media> old = s.media[kind];
        List<VmSettings.Media> out = new ArrayList<>();
        for (String u : VmHolder.media.get(kind)) {
            if (u == null) continue;
            VmSettings.Media keep = null;
            for (VmSettings.Media o : old) if (u.equals(o.uri)) keep = o;
            out.add(keep != null ? keep : new VmSettings.Media(u, kind == VmSettings.KIND_CD));
        }
        for (VmSettings.Media o : old) if (o.uri == null) out.add(o); /* disco novo ainda por criar */
        old.clear();
        old.addAll(out);
        s.save(act);
    }

    private VmSettings settings() {
        return VmHolder.vmId != null ? VmSettings.find(act, VmHolder.vmId) : null;
    }

    private boolean bios() {
        VmSettings s = settings();
        return s != null && s.bios && AppFiles.isX86(AppFiles.ARCHS[Math.max(0, Math.min(AppFiles.ARCHS.length - 1, s.arch))]);
    }

    private void toast(String s) {
        Toast.makeText(act, s, Toast.LENGTH_SHORT).show();
    }

    private int dp(int v) {
        return (int) (v * act.getResources().getDisplayMetrics().density + 0.5f);
    }

}
