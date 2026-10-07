package com.multivm.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.Menu;
import android.view.View;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.TextView;

import com.multivm.core.VirtualMachine;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Tela inicial: cada VM numa pilula com nome, resumo e estado. O botao de tres
 * pontos abre o menu da VM (iniciar, pausar/continuar, editar, excluir).
 */
public class HomeActivity extends Activity {

    /** extra do Intent: id da VM a iniciar assim que a tela abrir (vem do assistente) */
    static final String EXTRA_AUTOSTART = "autostart";

    private static final int MENU_START = 1, MENU_PAUSE = 2, MENU_EDIT = 3, MENU_DELETE = 4;

    private LinearLayout list;
    private TextView count;
    private View empty;
    /** pilulas por id, para atualizar o estado sem redesenhar a lista */
    private final Map<String, View> pills = new HashMap<>();
    private final Map<String, TextView> states = new HashMap<>();
    private String starting;
    private static final int REQ_MIC = 200;
    private VmSettings pendingMic;
    private boolean micAsked;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            updateStates();
            ui.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        /* primeira execucao: boas-vindas e assistente da primeira VM */
        if (VmSettings.isFirstRun(this)) {
            startActivity(new Intent(this, WizardActivity.class));
            finish();
            return;
        }
        setContentView(R.layout.activity_home);
        list = findViewById(R.id.vmList);
        count = findViewById(R.id.homeCount);
        empty = findViewById(R.id.homeEmpty);
        findViewById(R.id.homeNew).setOnClickListener(v ->
                startActivity(new Intent(this, WizardActivity.class).putExtra(WizardActivity.EXTRA_FROM_HOME, true)));
        if (saved == null) handleAutostart(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        handleAutostart(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (list == null) return;
        refresh();
        ui.post(tick);
    }

    @Override
    protected void onPause() {
        super.onPause();
        ui.removeCallbacks(tick);
    }

    private void handleAutostart(Intent intent) {
        String id = intent.getStringExtra(EXTRA_AUTOSTART);
        intent.removeExtra(EXTRA_AUTOSTART);
        VmSettings s = VmSettings.find(this, id);
        if (s != null) start(s);
    }

    /* ---- lista ---- */

    private void refresh() {
        list.removeAllViews();
        pills.clear();
        states.clear();
        List<VmSettings> vms = VmSettings.all(this);
        empty.setVisibility(vms.isEmpty() ? View.VISIBLE : View.GONE);
        count.setText(vms.isEmpty() ? "" : (vms.size() == 1 ? "1 máquina" : vms.size() + " máquinas")
                + " · toque para iniciar, ⋮ para editar");
        for (VmSettings s : vms) list.addView(pill(s));
        updateStates();
    }

    private View pill(VmSettings s) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setBackgroundResource(R.drawable.pill_bg);
        row.setPadding(dp(8), dp(8), dp(4), dp(8));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(10);
        row.setLayoutParams(lp);

        /* icone num circulo */
        ImageView icon = new ImageView(this);
        icon.setImageResource(s.bios ? R.drawable.ic_monitor : R.drawable.ic_terminal);
        icon.setImageTintList(ColorStateList.valueOf(getColor(R.color.accent)));
        icon.setBackgroundResource(R.drawable.circle_bg);
        icon.setPadding(dp(12), dp(12), dp(12), dp(12));
        row.addView(icon, new LinearLayout.LayoutParams(dp(48), dp(48)));

        LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        text.setPadding(dp(12), 0, dp(4), 0);
        TextView name = new TextView(this);
        name.setText(s.name);
        name.setSingleLine(true);
        name.setEllipsize(android.text.TextUtils.TruncateAt.END);
        name.setTextColor(getColor(R.color.text));
        name.setTextSize(16);
        name.setTypeface(Typeface.DEFAULT_BOLD);
        TextView sub = new TextView(this);
        sub.setText(s.summary());
        sub.setSingleLine(true);
        sub.setEllipsize(android.text.TextUtils.TruncateAt.END);
        sub.setTextColor(getColor(R.color.text2));
        sub.setTextSize(12);
        TextView state = new TextView(this);
        state.setTextSize(12);
        state.setCompoundDrawablesRelativeWithIntrinsicBounds(R.drawable.dot, 0, 0, 0);
        state.setCompoundDrawablePadding(dp(6));
        text.addView(name);
        text.addView(sub);
        text.addView(state);
        row.addView(text, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));

        ImageButton more = new ImageButton(this);
        more.setImageResource(R.drawable.ic_more);
        more.setBackgroundResource(R.drawable.round_btn_bg);
        more.setContentDescription("Opções de " + s.name);
        more.setOnClickListener(v -> showMenu(v, s));
        row.addView(more, new LinearLayout.LayoutParams(dp(44), dp(44)));

        /* toque: inicia a VM (ou volta para ela se ja estiver rodando); toque longo: menu */
        row.setOnClickListener(v -> {
            if (VmHolder.isRunning(s.id)) openVm();
            else start(s);
        });
        row.setOnLongClickListener(v -> {
            showMenu(more, s);
            return true;
        });
        pills.put(s.id, row);
        states.put(s.id, state);
        return row;
    }

    /** Estado de cada pilula: parada, iniciando, rodando, pausada ou desligada. */
    private void updateStates() {
        VirtualMachine vm = VmHolder.vm;
        for (Map.Entry<String, TextView> e : states.entrySet()) {
            String id = e.getKey();
            TextView t = e.getValue();
            String label;
            int color;
            boolean running = VmHolder.isRunning(id) && vm != null;
            if (id.equals(starting)) {
                label = "Iniciando…";
                color = R.color.accent;
            } else if (!running) {
                label = "Desligada";
                color = R.color.text2;
            } else if (vm.getState() == VirtualMachine.State.STOPPED) {
                label = "Parou — toque para ver";
                color = R.color.danger;
            } else if (vm.isPaused()) {
                label = "Pausada";
                color = R.color.warn;
            } else {
                label = "Em execução";
                color = R.color.ok;
            }
            t.setText(label);
            t.setTextColor(getColor(color == R.color.text2 ? R.color.text2 : color));
            t.setCompoundDrawableTintList(ColorStateList.valueOf(getColor(color)));
            View pill = pills.get(id);
            if (pill != null)
                pill.setBackgroundResource(running || id.equals(starting) ? R.drawable.pill_running_bg : R.drawable.pill_bg);
        }
    }

    /* ---- menu de tres pontos ---- */

    private void showMenu(View anchor, VmSettings s) {
        boolean running = VmHolder.isRunning(s.id);
        VirtualMachine vm = VmHolder.vm;
        boolean alive = running && vm != null && vm.getState() != VirtualMachine.State.STOPPED;
        PopupMenu pm = new PopupMenu(this, anchor, Gravity.END);
        Menu m = pm.getMenu();
        m.add(0, MENU_START, 0, running ? "Abrir tela da VM" : "Iniciar");
        m.add(0, MENU_PAUSE, 1, alive && vm.isPaused() ? "Continuar" : "Pausar").setEnabled(alive);
        m.add(0, MENU_EDIT, 2, "Editar");
        m.add(0, MENU_DELETE, 3, "Excluir").setEnabled(!running);
        pm.setOnMenuItemClickListener(item -> {
            switch (item.getItemId()) {
                case MENU_START:
                    if (running) openVm();
                    else start(s);
                    return true;
                case MENU_PAUSE:
                    togglePause();
                    return true;
                case MENU_EDIT:
                    edit(s);
                    return true;
                case MENU_DELETE:
                    askDelete(s);
                    return true;
            }
            return false;
        });
        pm.show();
    }

    private void start(VmSettings s) {
        if (starting != null) return;
        if (VmHolder.vm != null) {
            if (VmHolder.isRunning(s.id)) {
                openVm();
                return;
            }
            new AlertDialog.Builder(this)
                    .setTitle("Outra VM em execução")
                    .setMessage("“" + VmHolder.vmName + "” está rodando. Só uma VM roda por vez: pare-a antes de iniciar “"
                            + s.name + "”.")
                    .setPositiveButton("Abrir “" + VmHolder.vmName + "”", (d, w) -> openVm())
                    .setNegativeButton("Cancelar", null)
                    .show();
            return;
        }
        if (!micAsked && VmLauncher.needsMicPermission(this, s)) {
            pendingMic = s; /* inicia depois da resposta (negado = o convidado ouve silencio) */
            requestPermissions(new String[]{android.Manifest.permission.RECORD_AUDIO}, REQ_MIC);
            return;
        }
        micAsked = false;
        starting = s.id;
        updateStates();
        VmLauncher.start(this, s, err -> {
            starting = null;
            updateStates();
            if (err != null) {
                String msg = err.getMessage() != null ? err.getMessage() : err.toString();
                new AlertDialog.Builder(this).setTitle("Não foi possível iniciar “" + s.name + "”")
                        .setMessage(msg)
                        .setPositiveButton("Editar", (d, w) -> edit(s))
                        .setNegativeButton("OK", null)
                        .show();
            }
        });
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode != REQ_MIC || pendingMic == null)
            return;
        if (results.length == 0 || results[0] != android.content.pm.PackageManager.PERMISSION_GRANTED)
            android.widget.Toast.makeText(this, "Sem permissão do microfone: a VM vai ouvir silêncio.",
                    android.widget.Toast.LENGTH_LONG).show();
        VmSettings s = pendingMic;
        pendingMic = null;
        micAsked = true;
        start(s);
    }

    private void togglePause() {
        VirtualMachine vm = VmHolder.vm;
        if (vm == null) return;
        if (vm.isPaused()) vm.resume();
        else vm.pause();
        updateStates();
    }

    private void openVm() {
        startActivity(new Intent(this, VmActivity.class));
    }

    private void edit(VmSettings s) {
        startActivity(new Intent(this, MainActivity.class).putExtra(MainActivity.EXTRA_VM_ID, s.id));
    }

    private void askDelete(VmSettings s) {
        new AlertDialog.Builder(this)
                .setTitle("Excluir “" + s.name + "”?")
                .setMessage("A configuração da VM é apagada. Os arquivos de disco não são apagados (ficam em "
                        + AppFiles.diskDir(this) + ").")
                .setPositiveButton("Excluir", (d, w) -> {
                    VmSettings.delete(this, s.id);
                    refresh();
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }
}
