package com.multivm.app;

import android.app.AlertDialog;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import java.util.Locale;

/**
 * Configuracoes gerais (todas as VMs): tema, tela da VM, mouse e toque, teclado e discos.
 * Cada mudanca e salva na hora; a tela da VM aplica ao voltar.
 */
public class SettingsActivity extends BaseActivity {

    private AppSettings s;
    private LinearLayout root;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        s = AppSettings.load(this);
        build();
    }

    private void save() {
        s.save(this);
    }

    private void build() {
        ScrollView scroll = new ScrollView(this);
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(12), dp(16), dp(24));
        scroll.addView(root);
        setContentView(scroll);

        /* cabecalho */
        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        ImageButton back = new ImageButton(this);
        back.setImageResource(R.drawable.ic_back);
        back.setImageTintList(android.content.res.ColorStateList.valueOf(getColor(R.color.text)));
        back.setBackgroundResource(R.drawable.round_btn_bg);
        back.setContentDescription("Voltar");
        back.setOnClickListener(v -> finish());
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(dp(40), dp(40));
        bl.setMarginStart(-dp(8));
        head.addView(back, bl);
        TextView kicker = new TextView(this);
        kicker.setText("MULTIVM");
        kicker.setLetterSpacing(0.15f);
        kicker.setTextColor(getColor(R.color.accent));
        kicker.setTextSize(12);
        kicker.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        kicker.setPadding(dp(4), 0, 0, 0);
        head.addView(kicker);
        root.addView(head);
        TextView title = new TextView(this);
        title.setText("Configurações");
        title.setTextColor(getColor(R.color.text));
        title.setTextSize(28);
        title.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        root.addView(title);
        help(root, "Valem para todas as máquinas virtuais. As mudanças são salvas na hora.");

        /* aparencia */
        LinearLayout c = card("Aparência", R.drawable.ic_edit);
        choice(c, "Tema", AppSettings.THEME_NAMES, s.theme, i -> {
            if (i == s.theme) return;
            s.theme = i;
            save();
            recreate();
        });

        /* tela da VM */
        c = card("Tela da VM", R.drawable.ic_monitor);
        choice(c, "Escala da imagem", AppSettings.SCALE_NAMES, s.scale, i -> { s.scale = i; save(); });
        toggle(c, "Suavizar a imagem ampliada", "Desligado, os pixels ficam nítidos (bom para texto em baixa resolução).",
                s.smooth, b -> { s.smooth = b; save(); });
        int ri = java.util.Arrays.asList(AppSettings.RESOLUTIONS).indexOf(s.defaultResolution);
        choice(c, "Resolução padrão da tela gráfica (VMs novas com kernel Linux)", AppSettings.RESOLUTIONS,
                Math.max(0, ri), i -> { s.defaultResolution = AppSettings.RESOLUTIONS[i]; save(); });
        help(c, "Cada VM com kernel Linux pode ter a sua em Editar › Inicialização. Com BIOS (PC), a resolução é "
                + "escolhida dentro do sistema convidado (ex.: Painel de controle › Vídeo no Windows).");
        choice(c, "Taxa de atualização", AppSettings.FPS_NAMES, s.fps, i -> { s.fps = i; save(); });
        choice(c, "Orientação ao abrir a VM", AppSettings.ORIENT_NAMES, s.orientation, i -> { s.orientation = i; save(); });
        toggle(c, "Abrir a VM em tela cheia", "O botão Voltar alterna a tela cheia.", s.startFullscreen,
                b -> { s.startFullscreen = b; save(); });
        toggle(c, "Manter a tela do aparelho ligada", null, s.keepScreenOn, b -> { s.keepScreenOn = b; save(); });

        /* mouse e toque */
        c = card("Mouse e toque", R.drawable.ic_mouse);
        slider(c, "Velocidade do ponteiro", 1, 16, Math.round(s.mouseSpeed * 4), v -> v / 4f, v -> {
            s.mouseSpeed = v;
            save();
        });
        help(c, "O botão Mouse da barra da VM multiplica este valor (1x, 2x, 0,5x).");
        slider(c, "Velocidade da rolagem", 1, 16, Math.round(s.scrollSpeed * 4), v -> v / 4f, v -> {
            s.scrollSpeed = v;
            save();
        });
        toggle(c, "Inverter a rolagem", "Arrastar dois dedos para cima rola para baixo.", s.invertScroll,
                b -> { s.invertScroll = b; save(); });
        toggle(c, "Toque rápido = clique", null, s.tapClick, b -> { s.tapClick = b; save(); });
        toggle(c, "Toque com dois dedos = clique direito", null, s.twoFingerRight, b -> { s.twoFingerRight = b; save(); });
        toggle(c, "Segurar e arrastar = arrastar com o botão", "Mantenha o dedo parado até vibrar e arraste.",
                s.longPressDrag, b -> { s.longPressDrag = b; save(); });
        toggle(c, "Modo canhoto (troca os botões do toque)", null, s.leftHanded, b -> { s.leftHanded = b; save(); });
        toggle(c, "Vibrar ao começar a arrastar", null, s.haptics, b -> { s.haptics = b; save(); });
        toggle(c, "Mostrar o trackpad ao abrir a VM", null, s.trackpadOnStart, b -> { s.trackpadOnStart = b; save(); });
        help(c, "Um mouse USB ou Bluetooth também funciona direto na tela da VM.");

        /* teclado */
        c = card("Teclado", R.drawable.ic_keyboard);
        toggle(c, "Mostrar a barra de teclas especiais", "Esc, Tab, Ctrl, Alt, setas, F1–F12 e Ctrl+Alt+Del.",
                s.showKeys, b -> { s.showKeys = b; save(); });

        /* discos */
        c = card("Discos", R.drawable.ic_hdd);
        choice(c, "Ao criar um disco ou converter uma imagem", AppSettings.DISK_NAMES, s.diskLocation,
                i -> { s.diskLocation = i; save(); });
        help(c, "Perguntando, o Android abre o seletor “Salvar como” para escolher a pasta (Downloads, cartão SD…). "
                + "A pasta do app é " + AppFiles.diskDir(this) + " e é apagada se o app for desinstalado.");

        /* sobre */
        c = card("Sobre", R.drawable.ic_launcher);
        String ver;
        try {
            ver = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            ver = "?";
        }
        help(c, "MultiVM " + ver + " · núcleo " + com.multivm.core.MultiVm.version()
                + "\nEmulador de sistema completo: x86-64, i386, ARM64 e ARMv7.");

        Button reset = new Button(this, null, 0, R.style.TextButton);
        reset.setText("Restaurar os padrões");
        reset.setOnClickListener(v -> new AlertDialog.Builder(this)
                .setMessage("Voltar todas as configurações ao padrão?")
                .setPositiveButton("Restaurar", (d, w) -> {
                    AppSettings.reset(this);
                    recreate();
                })
                .setNegativeButton("Cancelar", null)
                .show());
        LinearLayout.LayoutParams rl = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(40));
        rl.topMargin = dp(16);
        rl.gravity = Gravity.CENTER_HORIZONTAL;
        root.addView(reset, rl);
    }

    /* ---- construtores de linhas ---- */

    private interface IntChange {
        void changed(int v);
    }

    private interface BoolChange {
        void changed(boolean v);
    }

    private interface FloatChange {
        void changed(float v);
    }

    private interface Mapper {
        float map(int v);
    }

    private LinearLayout card(String title, int icon) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackgroundResource(R.drawable.card_bg);
        c.setPadding(dp(16), dp(16), dp(16), dp(16));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(14);
        root.addView(c, lp);
        TextView t = new TextView(this, null, 0, R.style.CardTitle);
        t.setText(title);
        t.setCompoundDrawablesRelativeWithIntrinsicBounds(icon, 0, 0, 0);
        if (icon == R.drawable.ic_launcher) t.setCompoundDrawableTintList(null);
        c.addView(t);
        return c;
    }

    private TextView label(LinearLayout parent, String text) {
        TextView l = new TextView(this, null, 0, R.style.Label);
        l.setText(text);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(12);
        parent.addView(l, lp);
        return l;
    }

    private void help(LinearLayout parent, String text) {
        TextView h = new TextView(this);
        h.setText(text);
        h.setTextColor(getColor(R.color.text2));
        h.setTextSize(12);
        h.setPadding(0, dp(4), 0, 0);
        parent.addView(h);
    }

    private void choice(LinearLayout parent, String title, String[] items, int sel, IntChange change) {
        label(parent, title);
        Spinner sp = new Spinner(this);
        ArrayAdapter<String> ad = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, items);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad);
        sp.setSelection(sel, false);
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> p, View v, int pos, long id) { change.changed(pos); }

            @Override
            public void onNothingSelected(AdapterView<?> p) {}
        });
        parent.addView(sp, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    private void toggle(LinearLayout parent, String text, String helpText, boolean on, BoolChange change) {
        CheckBox cb = new CheckBox(this);
        cb.setText(text);
        cb.setChecked(on);
        cb.setOnCheckedChangeListener((b, c) -> change.changed(c));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(6);
        parent.addView(cb, lp);
        if (helpText != null) {
            TextView h = new TextView(this);
            h.setText(helpText);
            h.setTextColor(getColor(R.color.text2));
            h.setTextSize(12);
            h.setPadding(dp(32), 0, 0, 0);
            parent.addView(h);
        }
    }

    /** Barra de min a max (inteiros); map converte para o valor mostrado e salvo. */
    private void slider(LinearLayout parent, String title, int min, int max, int value, Mapper map, FloatChange change) {
        TextView l = label(parent, "");
        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        int v0 = Math.max(min, Math.min(max, value));
        bar.setProgress(v0 - min);
        l.setText(title + ": " + fmt(map.map(v0)) + "x");
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int p, boolean fromUser) {
                float f = map.map(p + min);
                l.setText(title + ": " + fmt(f) + "x");
                if (fromUser) change.changed(f);
            }

            @Override
            public void onStartTrackingTouch(SeekBar sb) {}

            @Override
            public void onStopTrackingTouch(SeekBar sb) {}
        });
        parent.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    private static String fmt(float f) {
        return f == (int) f ? Integer.toString((int) f) : String.format(Locale.ROOT, "%.2f", f).replaceAll("0+$", "").replace('.', ',');
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }
}
