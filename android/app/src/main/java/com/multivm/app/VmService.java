package com.multivm.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.drawable.Icon;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;

import com.multivm.core.ExitReason;
import com.multivm.core.VirtualMachine;

/**
 * Servico em primeiro plano que mantem a VM rodando com o app fora da tela (opcao "Continuar rodando a VM
 * em segundo plano"). Mostra uma notificacao fixa com Abrir, Pausar/Continuar e Parar e segura um wake
 * lock parcial enquanto a VM executa, para o aparelho nao dormir no meio.
 */
public final class VmService extends Service {

    static final String PERM_NOTIFY = "android.permission.POST_NOTIFICATIONS";

    private static final String CHANNEL = "vm_running";
    private static final int NOTIF_ID = 1;
    private static final String ACTION_TOGGLE_PAUSE = "com.multivm.app.TOGGLE_PAUSE";
    private static final String ACTION_STOP = "com.multivm.app.STOP";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private VirtualMachine vm;
    private PowerManager.WakeLock wake;
    private boolean stopping;

    private final VirtualMachine.StateListener listener = this::onVmState;

    /**
     * Liga ou desliga o servico conforme a opcao e a VM atual. Chamar na thread da UI com o app visivel
     * (o Android 12+ nao deixa iniciar servicos em primeiro plano a partir do segundo plano).
     */
    static void sync(Context c) {
        Context app = c.getApplicationContext();
        if (AppSettings.runInBackground(app) && vmAlive()) {
            Intent i = new Intent(app, VmService.class);
            try {
                if (Build.VERSION.SDK_INT >= 26) app.startForegroundService(i);
                else app.startService(i);
            } catch (RuntimeException e) {
                /* ex.: ForegroundServiceStartNotAllowedException; a VM continua sem a notificacao */
                android.util.Log.w("MultiVM", "servico em segundo plano nao iniciou", e);
            }
        } else {
            app.stopService(new Intent(app, VmService.class));
        }
    }

    private static boolean vmAlive() {
        VirtualMachine v = VmHolder.vm;
        if (v == null) return false;
        VirtualMachine.State st = v.getState();
        return st == VirtualMachine.State.RUNNING || st == VirtualMachine.State.PAUSED
                || st == VirtualMachine.State.CREATED;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, "VM em execução",
                    NotificationManager.IMPORTANCE_LOW);
            ch.setDescription("Aparece enquanto uma VM roda em segundo plano.");
            ch.setShowBadge(false);
            getSystemService(NotificationManager.class).createNotificationChannel(ch);
        }
        PowerManager pm = getSystemService(PowerManager.class);
        wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MultiVM:vm");
        wake.setReferenceCounted(false);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        /* startForeground primeiro: o Android exige em poucos segundos depois de startForegroundService */
        goForeground();
        String action = intent != null ? intent.getAction() : null;
        if (ACTION_STOP.equals(action)) {
            stopVm();
            return START_NOT_STICKY;
        }
        if (ACTION_TOGGLE_PAUSE.equals(action) && vm != null) {
            try {
                if (vm.isPaused()) vm.resume();
                else vm.pause();
                VmHolder.autoPaused = false;
            } catch (IllegalStateException ignored) {
                /* VM fechada enquanto a acao chegava */
            }
        }
        attach();
        refresh();
        return START_NOT_STICKY;
    }

    /** Acompanha a VM atual (troca de listener se outra VM foi iniciada). */
    private void attach() {
        VirtualMachine cur = VmHolder.vm;
        if (cur == vm) return;
        if (vm != null) vm.removeStateListener(listener);
        vm = cur;
        if (vm != null) vm.addStateListener(listener);
    }

    private void onVmState(VirtualMachine v, VirtualMachine.State state, ExitReason reason) {
        ui.post(() -> {
            if (v != vm) return;
            if (state == VirtualMachine.State.STOPPED || state == VirtualMachine.State.CLOSED) finish();
            else refresh();
        });
    }

    private boolean paused() {
        try {
            return vm != null && vm.isPaused();
        } catch (IllegalStateException e) {
            return false;
        }
    }

    private void refresh() {
        if (stopping) return;
        if (!vmAlive() || vm == null) {
            finish();
            return;
        }
        boolean p = paused();
        if (p) {
            if (wake.isHeld()) wake.release();
        } else if (!wake.isHeld()) {
            wake.acquire();
        }
        getSystemService(NotificationManager.class).notify(NOTIF_ID, build(p));
    }

    private void goForeground() {
        Notification n = build(paused());
        if (Build.VERSION.SDK_INT >= 34)
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else
            startForeground(NOTIF_ID, n);
    }

    private Notification build(boolean paused) {
        Notification.Builder b = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL)
                : new Notification.Builder(this).setPriority(Notification.PRIORITY_LOW);
        String name = VmHolder.vmName != null ? VmHolder.vmName : "VM";
        Intent open = new Intent(this, VmActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        b.setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(name)
                .setContentText(paused ? "Pausada · toque para abrir" : "Rodando em segundo plano · toque para abrir")
                .setOngoing(true)
                .setShowWhen(false)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_SERVICE)
                .setContentIntent(PendingIntent.getActivity(this, 0, open, pendingFlags()));
        b.addAction(action(R.drawable.ic_monitor, "Abrir",
                PendingIntent.getActivity(this, 1, open, pendingFlags())));
        b.addAction(action(paused ? R.drawable.ic_play : R.drawable.ic_pause, paused ? "Continuar" : "Pausar",
                service(ACTION_TOGGLE_PAUSE, 2)));
        b.addAction(action(R.drawable.ic_stop, "Parar", service(ACTION_STOP, 3)));
        if (Build.VERSION.SDK_INT >= 31)
            b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        return b.build();
    }

    private Notification.Action action(int icon, String title, PendingIntent pi) {
        return new Notification.Action.Builder(Icon.createWithResource(this, icon), title, pi).build();
    }

    private PendingIntent service(String action, int req) {
        Intent i = new Intent(this, VmService.class).setAction(action);
        return PendingIntent.getService(this, req, i, pendingFlags());
    }

    private static int pendingFlags() {
        return PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE;
    }

    /** Botao Parar: fecha a VM (como o Parar da tela da VM) e some com a notificacao. */
    private void stopVm() {
        stopping = true;
        if (vm != null) vm.removeStateListener(listener);
        vm = null;
        new Thread(() -> {
            VmHolder.closeCurrent();
            ui.post(this::finish);
        }, "MultiVM-close").start();
    }

    private void finish() {
        if (wake.isHeld()) wake.release();
        if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        if (vm != null) vm.removeStateListener(listener);
        vm = null;
        if (wake.isHeld()) wake.release();
        ui.removeCallbacksAndMessages(null);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
