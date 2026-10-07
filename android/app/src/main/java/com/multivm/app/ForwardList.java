package com.multivm.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.text.InputType;
import android.view.Gravity;
import android.widget.ArrayAdapter;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.util.List;

/**
 * Lista de redirecionamentos de porta (aparelho -> VM) com o dialogo de adicionar,
 * usada pelo editor da VM e pelo assistente. Os itens ficam em VmSettings.forwards
 * ("tcp:2222:22:local").
 */
final class ForwardList {

    private final Activity a;
    private final LinearLayout box;
    private final List<String> items;
    private final Runnable changed;
    private boolean noNet;

    ForwardList(Activity a, LinearLayout box, List<String> items, Runnable changed) {
        this.a = a;
        this.box = box;
        this.items = items;
        this.changed = changed;
    }

    /** Redesenha; noNet mostra "Sem rede" no lugar do exemplo. */
    void render(boolean noNet) {
        this.noNet = noNet;
        box.removeAllViews();
        int pad = (int) (8 * a.getResources().getDisplayMetrics().density);
        if (items.isEmpty()) {
            TextView t = new TextView(a);
            t.setText(noNet ? "Sem rede." : "Nenhum. Ex.: TCP 2222 → 22 para acessar o SSH da VM.");
            t.setTextColor(a.getColor(R.color.text2));
            t.setTextSize(13);
            box.addView(t);
        }
        for (int i = 0; i < items.size(); i++) {
            final int idx = i;
            LinearLayout row = new LinearLayout(a);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setBackgroundResource(R.drawable.item_bg);
            row.setPadding(pad + pad / 2, pad / 2, pad / 2, pad / 2);
            TextView t = new TextView(a);
            t.setText(VmSettings.forwardText(items.get(i)));
            t.setTextColor(a.getColor(R.color.text));
            row.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            ImageButton del = new ImageButton(a);
            del.setImageResource(R.drawable.ic_close);
            del.setBackgroundResource(R.drawable.toolbar_btn_bg);
            del.setContentDescription("Remover");
            del.setOnClickListener(v -> {
                items.remove(idx);
                render(this.noNet);
                if (changed != null) changed.run();
            });
            row.addView(del, new LinearLayout.LayoutParams(pad * 5, pad * 5));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.topMargin = pad / 2;
            box.addView(row, lp);
        }
    }

    void addDialog() {
        if (items.size() >= 16) {
            Toast.makeText(a, "no máximo 16 redirecionamentos", Toast.LENGTH_LONG).show();
            return;
        }
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (20 * a.getResources().getDisplayMetrics().density);
        v.setPadding(pad, pad / 2, pad, 0);
        Spinner proto = new Spinner(a);
        ArrayAdapter<String> ad = new ArrayAdapter<>(a, android.R.layout.simple_spinner_item, new String[]{"TCP", "UDP"});
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        proto.setAdapter(ad);
        EditText host = new EditText(a);
        host.setHint("porta no aparelho (1024 a 65535, ex.: 2222)");
        host.setInputType(InputType.TYPE_CLASS_NUMBER);
        EditText guest = new EditText(a);
        guest.setHint("porta na VM (ex.: 22)");
        guest.setInputType(InputType.TYPE_CLASS_NUMBER);
        CheckBox lan = new CheckBox(a);
        lan.setText("Aceitar conexões da rede local");
        v.addView(proto);
        v.addView(host);
        v.addView(guest);
        v.addView(lan);
        new AlertDialog.Builder(a)
                .setTitle("Redirecionar porta")
                .setView(v)
                .setPositiveButton("Adicionar", (d, w) -> {
                    String hp = host.getText().toString().trim();
                    String f = (proto.getSelectedItemPosition() == 1 ? "udp" : "tcp") + ":" + hp + ":"
                            + guest.getText().toString().trim() + ":" + (lan.isChecked() ? "lan" : "local");
                    int h;
                    try {
                        h = Integer.parseInt(hp);
                    } catch (NumberFormatException e) {
                        h = -1;
                    }
                    if (VmSettings.parseForward(f) == null || h < 1024) {
                        Toast.makeText(a, "portas inválidas: no aparelho use de 1024 a 65535 (o Android não deixa usar as "
                                + "menores); na VM, de 1 a 65535", Toast.LENGTH_LONG).show();
                        return;
                    }
                    items.add(f);
                    render(noNet);
                    if (changed != null) changed.run();
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }
}
