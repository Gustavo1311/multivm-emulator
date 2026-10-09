package com.multivm.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.text.InputType;
import android.view.Gravity;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;

import com.multivm.core.DiskImages;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Criacao de discos MVD com a escolha do local: a pasta do app ou qualquer pasta que o
 * usuario escolher no seletor do Android ("Salvar como"). A activity dona repassa
 * onActivityResult para {@link #onActivityResult}.
 */
final class DiskCreator {

    static final int REQ_SAVE = 950;

    interface Done {
        /** caminho (pasta do app) ou URI content:// (pasta escolhida) do disco criado */
        void created(String uri);
    }

    interface Pending {
        /** assistente: disco na pasta do app, criado so no fim */
        void pending(String name, long gb);
    }

    interface UriChosen {
        void chosen(Uri uri);
    }

    private final Activity a;
    private UriChosen waiting;

    DiskCreator(Activity a) {
        this.a = a;
    }

    /** Grupo "Salvar em": pasta do app ou escolher; o padrao vem de Configuracoes. */
    RadioGroup locationChooser() {
        RadioGroup g = new RadioGroup(a);
        g.setOrientation(RadioGroup.VERTICAL);
        TextView label = new TextView(a);
        label.setText("Salvar em");
        label.setTextColor(a.getColor(R.color.accent));
        label.setTextSize(13);
        label.setPadding(0, dp(12), 0, 0);
        g.addView(label);
        RadioButton ask = new RadioButton(a);
        ask.setId(android.R.id.button1);
        ask.setText("Escolher a pasta…");
        RadioButton app = new RadioButton(a);
        app.setId(android.R.id.button2);
        app.setText("Pasta do app (" + AppFiles.diskDir(a).getAbsolutePath() + ")");
        app.setTextSize(13);
        g.addView(ask);
        g.addView(app);
        g.check(AppSettings.load(a).diskLocation == AppSettings.DISK_APP ? app.getId() : ask.getId());
        return g;
    }

    static boolean choosesFolder(RadioGroup g) {
        return g.getCheckedRadioButtonId() == android.R.id.button1;
    }

    /**
     * Pergunta nome, tamanho e local de um disco novo. pending != null: o disco na pasta
     * do app nao e criado agora (assistente); senao e criado e entregue em done.
     */
    void ask(String title, String name, long gb, Pending pending, Done done) {
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(20), dp(10), dp(20), 0);
        EditText n = new EditText(a);
        n.setHint("nome do arquivo");
        n.setSingleLine(true);
        n.setText(name);
        EditText size = new EditText(a);
        size.setHint("tamanho máximo em GiB");
        size.setInputType(InputType.TYPE_CLASS_NUMBER);
        size.setText(String.valueOf(gb));
        TextView help = new TextView(a);
        help.setText("O arquivo MVD começa pequeno e cresce conforme o uso, até o tamanho máximo.");
        help.setTextColor(a.getColor(R.color.text2));
        help.setTextSize(12);
        RadioGroup where = locationChooser();
        box.addView(n);
        box.addView(size);
        box.addView(help);
        box.addView(where);
        android.widget.ScrollView sv = new android.widget.ScrollView(a);
        sv.addView(box);
        new AlertDialog.Builder(a)
                .setTitle(title)
                .setView(sv)
                .setPositiveButton("Criar", (d, w) -> {
                    long g;
                    try {
                        g = Long.parseLong(size.getText().toString().trim());
                    } catch (NumberFormatException e) {
                        g = -1;
                    }
                    String f = AppFiles.diskFileName(n.getText().toString());
                    if (f.isEmpty() || g <= 0) {
                        new AlertDialog.Builder(a).setMessage("Nome ou tamanho inválido.")
                                .setPositiveButton("OK", null).show();
                        return;
                    }
                    final long gib = g;
                    if (choosesFolder(where)) {
                        saveAs(f, uri -> createInDocument(uri, gib, done));
                    } else if (pending != null) {
                        pending.pending(f, gib);
                    } else {
                        createInApp(f, gib, done);
                    }
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    /** Abre o "Salvar como" do Android com o nome sugerido. */
    void saveAs(String name, UriChosen chosen) {
        Intent it = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        it.addCategory(Intent.CATEGORY_OPENABLE);
        it.setType("application/octet-stream");
        it.putExtra(Intent.EXTRA_TITLE, name);
        it.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        waiting = chosen;
        try {
            a.startActivityForResult(it, REQ_SAVE);
        } catch (Exception ex) {
            waiting = null;
            error("Não há seletor de pastas", ex);
        }
    }

    /** true se o resultado era deste ajudante. */
    boolean onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQ_SAVE) return false;
        UriChosen w = waiting;
        waiting = null;
        if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null && w != null) {
            AppFiles.takePersistable(a, data);
            w.chosen(data.getData());
        }
        return true;
    }

    private void createInApp(String name, long gb, Done done) {
        run("Criando " + name, () -> AppFiles.createMvd(a, name, gb), done);
    }

    /** O MVD vazio e criado no cache (poucos MiB) e copiado para o documento escolhido. */
    private void createInDocument(Uri doc, long gb, Done done) {
        run("Criando " + AppFiles.displayName(a, doc.toString()), () -> {
            File tmp = new File(a.getCacheDir(), "novo-" + System.nanoTime() + ".mvd");
            try {
                DiskImages.createMvd(tmp.getAbsolutePath(), gb << 30);
                copyToDocument(tmp, doc, null);
            } catch (IOException | RuntimeException ex) {
                deleteDocument(doc);
                throw ex;
            } finally {
                tmp.delete();
            }
            return doc.toString();
        }, done);
    }

    interface Progress {
        void progress(long done, long total);
    }

    /** Copia um arquivo local para um documento do SAF (substitui o conteudo). */
    void copyToDocument(File src, Uri doc, Progress p) throws IOException {
        try (ParcelFileDescriptor pfd = a.getContentResolver().openFileDescriptor(doc, "rw")) {
            if (pfd == null) throw new IOException("não consegui abrir o arquivo de destino");
            try (InputStream in = new FileInputStream(src);
                 FileOutputStream os = new FileOutputStream(pfd.getFileDescriptor())) {
                os.getChannel().truncate(0);
                byte[] buf = new byte[1 << 20];
                long total = src.length(), done = 0;
                int r;
                while ((r = in.read(buf)) > 0) {
                    os.write(buf, 0, r);
                    done += r;
                    if (p != null) p.progress(done, total);
                }
                os.getFD().sync();
            }
        }
    }

    /** Apaga um documento criado pelo "Salvar como" (falha ou cancelamento). */
    void deleteDocument(Uri doc) {
        try {
            android.provider.DocumentsContract.deleteDocument(a.getContentResolver(), doc);
        } catch (Exception ignored) {
        }
    }

    private interface Job {
        String run() throws Exception;
    }

    private void run(String title, Job job, Done done) {
        LinearLayout box = new LinearLayout(a);
        box.setGravity(Gravity.CENTER_VERTICAL);
        box.setPadding(dp(20), dp(16), dp(20), dp(16));
        box.addView(new ProgressBar(a));
        TextView msg = new TextView(a);
        msg.setText("Aguarde…");
        msg.setPadding(dp(16), 0, 0, 0);
        box.addView(msg);
        AlertDialog dlg = new AlertDialog.Builder(a).setTitle(title).setView(box).setCancelable(false).show();
        new Thread(() -> {
            String uri = null;
            Exception err = null;
            try {
                uri = job.run();
            } catch (Exception ex) {
                err = ex;
            }
            final String u = uri;
            final Exception e = err;
            a.runOnUiThread(() -> {
                dlg.dismiss();
                if (e != null) error("Falha ao criar o disco", e);
                else done.created(u);
            });
        }, "disk-create").start();
    }

    private void error(String title, Exception ex) {
        String m = ex.getMessage() != null ? ex.getMessage() : ex.toString();
        new AlertDialog.Builder(a).setTitle(title).setMessage(m).setPositiveButton("OK", null).show();
    }

    private int dp(int v) {
        return (int) (v * a.getResources().getDisplayMetrics().density);
    }
}
