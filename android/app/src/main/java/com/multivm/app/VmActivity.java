package com.multivm.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import com.multivm.core.ExitReason;
import com.multivm.core.KeyMapper;
import com.multivm.core.VirtualMachine;

import java.util.Locale;

/** Tela da VM em execucao: video, terminal serial e controles. */
public class VmActivity extends Activity implements ScreenView.InputSink {

    /* codigos evdev */
    private static final int KEY_ESC = 1, KEY_BACKSPACE = 14, KEY_TAB = 15, KEY_ENTER = 28, KEY_LEFTCTRL = 29,
            KEY_LEFTSHIFT = 42, KEY_LEFTALT = 56, KEY_F1 = 59, KEY_F11 = 87, KEY_F12 = 88, KEY_HOME = 102,
            KEY_UP = 103, KEY_PAGEUP = 104, KEY_LEFT = 105, KEY_RIGHT = 106, KEY_END = 107, KEY_DOWN = 108,
            KEY_PAGEDOWN = 109, KEY_INSERT = 110, KEY_DELETE = 111, KEY_LEFTMETA = 125;

    private static final float[] SENSITIVITY = {1f, 2f, 0.5f};

    private VirtualMachine vm;
    private TerminalBuffer terminal;
    private ScreenView screen;
    private View terminalView, keyBar;
    private TextView termText, status;
    private View statusDot;
    private ScrollView termScroll;
    private EditText termInput;
    private Button btnView, btnPause, btnMouse;
    private boolean showTerminal, muted;
    private int sensIdx;
    private MediaPanel mediaPanel;
    private TrackpadView trackpad;
    private Button btnTrackpad, btnRotate;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final TerminalBuffer.Snapshot seen = new TerminalBuffer.Snapshot();
    private final boolean[] replace = new boolean[1];
    private final KeyCharacterMap charMap = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);

    /* modificadores "presos" da barra de teclas: valem para a proxima tecla */
    private final int[] stickyCodes = {KEY_LEFTCTRL, KEY_LEFTALT, KEY_LEFTSHIFT, KEY_LEFTMETA};
    private final Button[] stickyButtons = new Button[stickyCodes.length];
    private final boolean[] sticky = new boolean[stickyCodes.length];

    private long lastInsns, lastStatusTime;
    private boolean ended;

    private final VirtualMachine.StateListener stateListener = (v, state, reason) ->
            ui.post(() -> onVmState(state, reason));

    private final Runnable frame = new Runnable() {
        @Override
        public void run() {
            if (vm == null) return;
            if (!showTerminal) screen.refresh(vm);
            else pumpTerminal();
            ui.postDelayed(this, 33);
        }
    };

    private final Runnable statusTick = new Runnable() {
        @Override
        public void run() {
            if (vm == null) return;
            updateStatus();
            if (showTerminal) pumpTerminal();
            ui.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        vm = VmHolder.vm;
        terminal = VmHolder.terminal;
        if (vm == null) {
            finish();
            return;
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(R.layout.activity_vm);

        screen = findViewById(R.id.screen);
        terminalView = findViewById(R.id.terminal);
        keyBar = findViewById(R.id.keyBar);
        termText = findViewById(R.id.termText);
        termScroll = findViewById(R.id.termScroll);
        termInput = findViewById(R.id.termInput);
        status = findViewById(R.id.status);
        statusDot = findViewById(R.id.statusDot);
        btnView = findViewById(R.id.btnView);
        btnPause = findViewById(R.id.btnPause);
        btnMouse = findViewById(R.id.btnMouse);

        screen.setInputSink(this);
        btnView.setOnClickListener(v -> setTerminal(!showTerminal));
        findViewById(R.id.btnKbd).setOnClickListener(v -> toggleKeyboard());
        btnPause.setOnClickListener(v -> {
            if (vm.isPaused()) vm.resume();
            else vm.pause();
            btnPause.setText(vm.isPaused() ? "Continuar" : "Pausar");
            setTopIcon(btnPause, vm.isPaused() ? R.drawable.ic_play : R.drawable.ic_pause);
            updateStatus();
        });
        Button btnSound = findViewById(R.id.btnSound);
        VirtualMachine.AudioState as = vm.getAudioState();
        if (as == VirtualMachine.AudioState.NONE) {
            btnSound.setVisibility(View.GONE);
        } else {
            btnSound.setOnClickListener(v -> {
                if (vm.getAudioState() == VirtualMachine.AudioState.UNSUPPORTED) {
                    new AlertDialog.Builder(this).setMessage("O som precisa do Android 8 ou mais novo (AAudio).")
                            .setPositiveButton("OK", null).show();
                    return;
                }
                muted = !muted;
                vm.setAudioMuted(muted);
                btnSound.setText(muted ? "Mudo" : "Som");
                setTopIcon(btnSound, muted ? R.drawable.ic_volume_off : R.drawable.ic_volume);
            });
            if (as == VirtualMachine.AudioState.UNSUPPORTED) {
                btnSound.setText("Sem som");
                setTopIcon(btnSound, R.drawable.ic_volume_off);
            }
        }
        findViewById(R.id.btnReset).setOnClickListener(v -> confirm("Reiniciar a VM?", vm::reset));
        findViewById(R.id.btnPower).setOnClickListener(v -> vm.pressPowerButton());
        findViewById(R.id.btnStop).setOnClickListener(v -> askStop());
        btnMouse.setOnClickListener(v -> {
            sensIdx = (sensIdx + 1) % SENSITIVITY.length;
            screen.setSensitivity(SENSITIVITY[sensIdx]);
            btnMouse.setText("Mouse " + fmt(SENSITIVITY[sensIdx]) + "x");
        });
        btnMouse.setText("Mouse 1x");

        mediaPanel = new MediaPanel(this, vm);
        findViewById(R.id.btnMedia).setOnClickListener(v -> mediaPanel.show());
        btnTrackpad = findViewById(R.id.btnTrackpad);
        btnTrackpad.setOnClickListener(v -> setTrackpad(trackpad == null));
        btnRotate = findViewById(R.id.btnRotate);
        btnRotate.setOnClickListener(v -> rotate());
        btnRotate.setOnLongClickListener(v -> {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
            btnRotate.setText("Girar");
            android.widget.Toast.makeText(this, "Rotação automática", android.widget.Toast.LENGTH_SHORT).show();
            return true;
        });

        termInput.setOnEditorActionListener((tv, actionId, ev) -> {
            boolean enter = actionId == EditorInfo.IME_ACTION_SEND
                    || (ev != null && ev.getKeyCode() == KeyEvent.KEYCODE_ENTER && ev.getAction() == KeyEvent.ACTION_DOWN);
            if (!enter) return false;
            vm.sendSerial(termInput.getText().toString() + "\r");
            termInput.setText("");
            return true;
        });
        findViewById(R.id.termCtrlC).setOnClickListener(v -> vm.sendSerial("\u0003"));
        findViewById(R.id.termTab).setOnClickListener(v -> vm.sendSerial("\t"));
        findViewById(R.id.termUp).setOnClickListener(v -> vm.sendSerial("\u001b[A"));
        findViewById(R.id.termClear).setOnClickListener(v -> terminal.clear());

        buildKeyBar();
        vm.addStateListener(stateListener);
        setTerminal(!VmHolder.hasScreen);
        if (vm.getState() == VirtualMachine.State.STOPPED) onVmState(vm.getState(), vm.getLastExitReason());
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (vm == null) return;
        /* a VM pode ter sido pausada/continuada pela tela inicial */
        btnPause.setText(vm.isPaused() ? "Continuar" : "Pausar");
        setTopIcon(btnPause, vm.isPaused() ? R.drawable.ic_play : R.drawable.ic_pause);
        ui.post(frame);
        ui.post(statusTick);
    }

    @Override
    protected void onPause() {
        super.onPause();
        ui.removeCallbacks(frame);
        ui.removeCallbacks(statusTick);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (vm != null) vm.removeStateListener(stateListener);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == MediaPanel.REQ_PICK && resultCode == RESULT_OK && mediaPanel != null)
            mediaPanel.onPicked(data);
    }

    @Override
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        if (trackpad != null) trackpad.keepInside();
    }

    @Override
    public void onBackPressed() {
        askStop();
    }

    /* ---- controles ---- */

    private static void setTopIcon(Button b, int icon) {
        b.setCompoundDrawablesRelativeWithIntrinsicBounds(0, icon, 0, 0);
    }

    private static String fmt(float f) {
        return f == (int) f ? Integer.toString((int) f) : Float.toString(f);
    }

    /** Mostra/esconde o trackpad flutuante sobre a tela da VM. */
    private void setTrackpad(boolean on) {
        FrameLayout area = findViewById(R.id.vmArea);
        if (on && trackpad == null) {
            if (showTerminal) setTerminal(false);
            trackpad = new TrackpadView(this, screen, () -> setTrackpad(false));
            trackpad.place(area);
        } else if (!on && trackpad != null) {
            trackpad.release();
            area.removeView(trackpad);
            trackpad = null;
        }
        btnTrackpad.setTextColor(getColor(on ? R.color.accent : R.color.text2));
        btnTrackpad.setCompoundDrawableTintList(on
                ? android.content.res.ColorStateList.valueOf(getColor(R.color.accent)) : null);
    }

    /** Alterna paisagem/retrato (toque longo: volta a girar com o aparelho). */
    private void rotate() {
        boolean landscape = getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
        setRequestedOrientation(landscape ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
                : ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        btnRotate.setText(landscape ? "Retrato" : "Paisagem");
    }

    private void confirm(String msg, Runnable action) {
        new AlertDialog.Builder(this)
                .setMessage(msg)
                .setPositiveButton("Sim", (d, w) -> action.run())
                .setNegativeButton("Não", null)
                .show();
    }

    private void askStop() {
        if (ended) {
            closeVm();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("Parar a VM")
                .setMessage("Desligar à força perde o que não foi gravado no disco do convidado. "
                        + "O botão Energia pede um desligamento ordenado (ACPI, x86).")
                .setPositiveButton("Parar", (d, w) -> closeVm())
                .setNeutralButton("Segundo plano", (d, w) -> moveTaskToBack(true))
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void closeVm() {
        ui.removeCallbacks(frame);
        ui.removeCallbacks(statusTick);
        vm.removeStateListener(stateListener);
        vm = null;
        status.setText("parando...");
        new Thread(() -> {
            VmHolder.closeCurrent();
            ui.post(this::finish);
        }, "MultiVM-close").start();
    }

    private void onVmState(VirtualMachine.State state, ExitReason reason) {
        if (vm == null || state != VirtualMachine.State.STOPPED || ended) return;
        ended = true;
        updateStatus();
        pumpTerminal();
        String why = reason == ExitReason.SHUTDOWN ? "o sistema convidado desligou a máquina"
                : reason == ExitReason.ERROR ? "erro fatal na emulação (veja o logcat, tag MultiVM)"
                : "execução interrompida";
        new AlertDialog.Builder(this)
                .setTitle("VM parada")
                .setMessage(why)
                .setPositiveButton("Fechar", (d, w) -> closeVm())
                .setNegativeButton("Ver tela", null)
                .show();
    }

    private void updateStatus() {
        if (vm == null) return;
        long now = SystemClock.elapsedRealtime();
        long insns = vm.getInstructionCount();
        String mips = "";
        if (lastStatusTime != 0 && now > lastStatusTime) {
            double m = (insns - lastInsns) / 1000.0 / (now - lastStatusTime);
            mips = String.format(Locale.ROOT, "%.0f MIPS", m);
        }
        lastInsns = insns;
        lastStatusTime = now;
        StringBuilder sb = new StringBuilder();
        if (VmHolder.vmName != null) sb.append(VmHolder.vmName).append("  ");
        sb.append(vm.getConfig().getArchitecture().id()).append("  ");
        VirtualMachine.State st = vm.getState();
        boolean paused = st == VirtualMachine.State.RUNNING && vm.isPaused();
        sb.append(paused ? "PAUSADA" : st.name());
        int dot = ended || st == VirtualMachine.State.STOPPED ? R.color.danger
                : paused ? R.color.warn : st == VirtualMachine.State.RUNNING ? R.color.ok : R.color.text2;
        statusDot.setBackgroundTintList(android.content.res.ColorStateList.valueOf(getColor(dot)));
        if (!mips.isEmpty() && !ended) sb.append("  ").append(mips);
        long lag = vm.getClockLagNanos() / 1_000_000;
        if (lag > 100) sb.append(String.format(Locale.ROOT, "  atraso %.1f s", lag / 1000.0));
        String res = screen.resolution();
        if (res != null) sb.append("  ").append(res);
        if (vm.getConfig().hasMicrophone()) sb.append("  🎤");
        if (vm.isVncRunning()) {
            VmSettings s = VmSettings.find(this, VmHolder.vmId);
            String host = s != null && s.vncLan ? VmLauncher.lanAddress() : "127.0.0.1";
            int port = s != null ? VmSettings.vncTcpPort(s.vncPort) : 5900;
            int clients = vm.getVncClients();
            sb.append("  VNC ").append(host != null ? host : "?").append("::").append(port);
            if (clients > 0) sb.append(" (").append(clients).append(clients == 1 ? " cliente)" : " clientes)");
        } else if (VmHolder.vncError != null) {
            sb.append("  VNC: erro (").append(VmHolder.vncError).append(')');
        }
        status.setText(sb);
        status.setTextColor(getColor(VmHolder.vncError != null && !vm.isVncRunning() ? R.color.danger : R.color.text2));
    }

    private void setTerminal(boolean on) {
        if (on && trackpad != null) setTrackpad(false);
        showTerminal = on;
        terminalView.setVisibility(on ? View.VISIBLE : View.GONE);
        screen.setVisibility(on ? View.GONE : View.VISIBLE);
        keyBar.setVisibility(on ? View.GONE : View.VISIBLE);
        btnView.setText(on ? "Tela" : "Terminal");
        setTopIcon(btnView, on ? R.drawable.ic_monitor : R.drawable.ic_terminal);
        if (on) {
            pumpTerminal();
            termInput.requestFocus();
        } else {
            screen.requestFocus();
        }
    }

    private void toggleKeyboard() {
        InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        View target = showTerminal ? termInput : screen;
        target.requestFocus();
        imm.toggleSoftInput(InputMethodManager.SHOW_FORCED, 0);
    }

    private void pumpTerminal() {
        if (terminal == null) return;
        String s = terminal.snapshot(seen, replace);
        if (s == null) return;
        boolean atBottom = !termScroll.canScrollVertically(1);
        if (replace[0] || termText.length() > 96 * 1024) termText.setText(replace[0] ? s : terminalAll());
        else termText.append(s);
        if (atBottom || replace[0]) termScroll.post(() -> termScroll.fullScroll(View.FOCUS_DOWN));
    }

    private String terminalAll() {
        seen.rewrites = -1;
        return terminal.snapshot(seen, replace);
    }

    /* ---- barra de teclas ---- */

    private void buildKeyBar() {
        LinearLayout row = findViewById(R.id.keyRow);
        addKey(row, "Esc", KEY_ESC);
        addKey(row, "Tab", KEY_TAB);
        String[] names = {"Ctrl", "Alt", "Shift", "Win"};
        for (int i = 0; i < stickyCodes.length; i++) {
            final int idx = i;
            Button b = keyButton(row, names[i]);
            stickyButtons[i] = b;
            b.setOnClickListener(v -> {
                sticky[idx] = !sticky[idx];
                updateSticky();
            });
        }
        addKey(row, "←", KEY_LEFT);
        addKey(row, "↑", KEY_UP);
        addKey(row, "↓", KEY_DOWN);
        addKey(row, "→", KEY_RIGHT);
        addKey(row, "Enter", KEY_ENTER);
        addKey(row, "⌫", KEY_BACKSPACE);
        addKey(row, "Del", KEY_DELETE);
        addKey(row, "Home", KEY_HOME);
        addKey(row, "End", KEY_END);
        addKey(row, "PgUp", KEY_PAGEUP);
        addKey(row, "PgDn", KEY_PAGEDOWN);
        addKey(row, "Ins", KEY_INSERT);
        Button cad = keyButton(row, "Ctrl+Alt+Del");
        cad.setOnClickListener(v -> {
            vm.sendKeyEvdev(KEY_LEFTCTRL, true);
            vm.sendKeyEvdev(KEY_LEFTALT, true);
            tap(KEY_DELETE);
            vm.sendKeyEvdev(KEY_LEFTALT, false);
            vm.sendKeyEvdev(KEY_LEFTCTRL, false);
        });
        for (int i = 0; i < 12; i++) addKey(row, "F" + (i + 1), i < 10 ? KEY_F1 + i : (i == 10 ? KEY_F11 : KEY_F12));
    }

    private Button keyButton(LinearLayout row, String label) {
        Button b = new Button(this, null, 0, R.style.KeyButton);
        b.setText(label);
        b.setAllCaps(false);
        row.addView(b, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                (int) (40 * getResources().getDisplayMetrics().density)));
        return b;
    }

    private void addKey(LinearLayout row, String label, int code) {
        keyButton(row, label).setOnClickListener(v -> typeKey(code));
    }

    private void updateSticky() {
        for (int i = 0; i < sticky.length; i++)
            stickyButtons[i].setTextColor(getColor(sticky[i] ? R.color.accent : R.color.text));
    }

    private void tap(int code) {
        vm.sendKeyEvdev(code, true);
        vm.sendKeyEvdev(code, false);
    }

    /** Aperta e solta uma tecla com os modificadores presos, e os solta em seguida. */
    private void typeKey(int code) {
        if (vm == null) return;
        boolean any = false;
        for (int i = 0; i < sticky.length; i++) if (sticky[i]) { vm.sendKeyEvdev(stickyCodes[i], true); any = true; }
        tap(code);
        for (int i = sticky.length - 1; i >= 0; i--) if (sticky[i]) vm.sendKeyEvdev(stickyCodes[i], false);
        if (any) {
            java.util.Arrays.fill(sticky, false);
            updateSticky();
        }
    }

    /* ---- ScreenView.InputSink ---- */

    @Override
    public boolean onHardKey(int keyCode, boolean down) {
        if (vm == null) return false;
        if (!down) return vm.sendKey(keyCode, false);
        int code = KeyMapper.toEvdev(keyCode);
        if (code <= 0) return false;
        boolean any = false;
        for (boolean s : sticky) any |= s;
        if (any) typeKey(code);           /* tecla do teclado virtual com Ctrl/Alt presos */
        else vm.sendKeyEvdev(code, true);
        return true;
    }

    @Override
    public void onChar(char c) {
        if (vm == null) return;
        if (c == '\n') {
            typeKey(KEY_ENTER);
            return;
        }
        KeyEvent[] evs = charMap.getEvents(new char[]{c});
        if (evs == null) return;
        boolean any = false;
        for (boolean s : sticky) any |= s;
        if (any) {
            /* Ctrl+letra etc.: usa so a tecla principal */
            for (KeyEvent e : evs) {
                int code = KeyMapper.toEvdev(e.getKeyCode());
                if (e.getAction() == KeyEvent.ACTION_DOWN && code > 0 && code != KEY_LEFTSHIFT && code != 54) {
                    typeKey(code);
                    return;
                }
            }
            return;
        }
        for (KeyEvent e : evs) vm.sendKey(e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN);
    }

    @Override
    public void onPointer(int dx, int dy, int wheel, int buttons) {
        if (vm != null) vm.sendPointer(dx, dy, wheel, buttons);
    }
}
