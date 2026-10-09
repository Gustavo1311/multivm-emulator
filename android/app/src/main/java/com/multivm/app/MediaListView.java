package com.multivm.app;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * Lista de midias de um tipo (discos rigidos, CD/DVD ou disquetes): um item por
 * arquivo, com "somente leitura" e remover, e os botoes de adicionar embaixo. A
 * escolha de arquivos e a criacao de discos ficam com a activity ({@link Host}).
 */
public class MediaListView extends LinearLayout {

    interface Host {
        /** Abrir o seletor de arquivos; o resultado volta por {@link #add(String)}. */
        void pickMedia(MediaListView list);

        /** Disco novo (so discos rigidos). */
        void newDisk(MediaListView list);

        /** Converter uma imagem para MVD (so discos rigidos; null = sem o botao). */
        void convertDisk(MediaListView list);

        /** Tocar num disco novo pendente (assistente): editar nome/tamanho. */
        void editPending(MediaListView list, int index);

        void onMediaChanged(MediaListView list);
    }

    private static final String[] TITLES = {"Discos rígidos", "CD/DVD (ISO)", "Disquetes"};
    private static final String[] EMPTY = {"Nenhum disco rígido", "Nenhuma ISO", "Nenhum disquete"};
    private static final int[] ICONS = {R.drawable.ic_hdd, R.drawable.ic_disc, R.drawable.ic_floppy};

    private int kind;
    private int max;
    private Host host;
    private boolean showConvert = true;
    private final List<VmSettings.Media> items = new ArrayList<>();

    public MediaListView(Context c) {
        super(c);
        setOrientation(VERTICAL);
    }

    public MediaListView(Context c, AttributeSet a) {
        super(c, a);
        setOrientation(VERTICAL);
    }

    void setup(int kind, Host host) {
        this.kind = kind;
        this.max = VmSettings.MAX[kind];
        this.host = host;
        render();
    }

    void setShowConvert(boolean show) {
        showConvert = show;
        render();
    }

    int kind() { return kind; }

    List<VmSettings.Media> items() { return items; }

    void setItems(List<VmSettings.Media> list) {
        items.clear();
        items.addAll(list);
        render();
    }

    boolean isFull() { return items.size() >= max; }

    void add(String uri) {
        if (isFull()) return;
        items.add(new VmSettings.Media(uri, kind == VmSettings.KIND_CD));
        changed();
    }

    void addPending(String name, long gb) {
        if (isFull()) return;
        items.add(VmSettings.Media.pending(name, gb));
        changed();
    }

    void changed() {
        render();
        if (host != null) host.onMediaChanged(this);
    }

    /* ---- desenho ---- */

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }

    private String itemLabel(int i) {
        if (kind == VmSettings.KIND_FLOPPY) return i == 0 ? "A:" : "B:";
        return (kind == VmSettings.KIND_CD ? "CD " : "Disco ") + (i + 1);
    }

    void render() {
        removeAllViews();
        Context c = getContext();

        /* titulo: icone, nome e contagem */
        LinearLayout head = new LinearLayout(c);
        head.setGravity(Gravity.CENTER_VERTICAL);
        ImageView hi = new ImageView(c);
        hi.setImageResource(ICONS[kind]);
        hi.setImageTintList(ColorStateList.valueOf(getResources().getColor(R.color.accent, null)));
        head.addView(hi, new LayoutParams(dp(20), dp(20)));
        TextView ht = new TextView(c);
        ht.setText(TITLES[kind]);
        ht.setTextColor(getResources().getColor(R.color.text, null));
        ht.setTypeface(Typeface.DEFAULT_BOLD);
        ht.setPadding(dp(8), 0, 0, 0);
        head.addView(ht, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
        TextView count = new TextView(c);
        count.setText(items.size() + " de " + max);
        count.setTextColor(getResources().getColor(R.color.text2, null));
        count.setTextSize(12);
        head.addView(count);
        LayoutParams hlp = new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
        hlp.topMargin = dp(12);
        addView(head, hlp);

        if (items.isEmpty()) {
            TextView e = new TextView(c);
            e.setText(EMPTY[kind]);
            e.setTextColor(getResources().getColor(R.color.text2, null));
            e.setTextSize(13);
            e.setPadding(dp(28), dp(6), 0, dp(2));
            addView(e);
        }
        for (int i = 0; i < items.size(); i++) addView(itemRow(i));

        if (!isFull() && host != null) {
            LinearLayout bar = new LinearLayout(c);
            bar.addView(smallButton(kind == VmSettings.KIND_DISK ? "Adicionar imagem" : "Adicionar",
                    R.drawable.ic_add, v -> host.pickMedia(this)));
            if (kind == VmSettings.KIND_DISK) {
                bar.addView(smallButton("Novo disco", R.drawable.ic_wizard, v -> host.newDisk(this)));
                if (showConvert)
                    bar.addView(smallButton("Converter", R.drawable.ic_convert, v -> host.convertDisk(this)));
            }
            /* rola na horizontal em telas estreitas */
            android.widget.HorizontalScrollView hs = new android.widget.HorizontalScrollView(c);
            hs.setHorizontalScrollBarEnabled(false);
            hs.addView(bar);
            LayoutParams blp = new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
            blp.topMargin = dp(8);
            addView(hs, blp);
        }
    }

    private View itemRow(int i) {
        Context c = getContext();
        VmSettings.Media m = items.get(i);
        LinearLayout row = new LinearLayout(c);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setBackgroundResource(R.drawable.item_bg);
        row.setPadding(dp(12), dp(6), dp(4), dp(6));

        LinearLayout text = new LinearLayout(c);
        text.setOrientation(VERTICAL);
        TextView name = new TextView(c);
        name.setSingleLine(true);
        name.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        name.setTextColor(getResources().getColor(R.color.text, null));
        TextView sub = new TextView(c);
        sub.setTextSize(12);
        sub.setTextColor(getResources().getColor(R.color.text2, null));
        if (m.isPending()) {
            name.setText(m.newName);
            sub.setText(itemLabel(i) + " · novo, até " + m.newGb + " GiB · toque para editar");
            row.setOnClickListener(v -> host.editPending(this, i));
        } else {
            name.setText(AppFiles.displayName(c, m.uri));
            sub.setText(itemLabel(i) + (m.ro && kind != VmSettings.KIND_CD ? " · somente leitura" : ""));
        }
        text.addView(name);
        text.addView(sub);
        row.addView(text, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));

        if (kind != VmSettings.KIND_CD && !m.isPending()) {
            CheckBox ro = new CheckBox(c);
            ro.setButtonDrawable(R.drawable.ic_lock);
            ro.setButtonTintList(new ColorStateList(
                    new int[][]{{android.R.attr.state_checked}, {}},
                    new int[]{getResources().getColor(R.color.warn, null),
                            getResources().getColor(R.color.outline, null)}));
            ro.setChecked(m.ro);
            ro.setContentDescription("Somente leitura");
            ro.setPadding(dp(4), 0, dp(4), 0);
            ro.setOnCheckedChangeListener((b, on) -> {
                m.ro = on;
                changed();
            });
            row.addView(ro);
        }
        ImageButton del = new ImageButton(c);
        del.setImageResource(R.drawable.ic_close);
        del.setImageTintList(ColorStateList.valueOf(getResources().getColor(R.color.text2, null)));
        del.setBackgroundResource(R.drawable.toolbar_btn_bg);
        del.setContentDescription("Remover");
        del.setOnClickListener(v -> {
            items.remove(i);
            changed();
        });
        row.addView(del, new LayoutParams(dp(40), dp(40)));

        LayoutParams lp = new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(6);
        row.setLayoutParams(lp);
        return row;
    }

    private Button smallButton(String text, int icon, OnClickListener l) {
        Button b = new Button(getContext(), null, 0, R.style.OutlineButton);
        b.setText(text);
        b.setCompoundDrawablesRelativeWithIntrinsicBounds(icon, 0, 0, 0);
        b.setCompoundDrawableTintList(ColorStateList.valueOf(getResources().getColor(R.color.accent, null)));
        b.setOnClickListener(l);
        LayoutParams lp = new LayoutParams(LayoutParams.WRAP_CONTENT, dp(40));
        lp.setMarginEnd(dp(6));
        b.setLayoutParams(lp);
        return b;
    }
}
