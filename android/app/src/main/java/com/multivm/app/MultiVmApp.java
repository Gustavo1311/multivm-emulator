package com.multivm.app;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.os.Bundle;

import com.multivm.core.VirtualMachine;

/**
 * Sabe quando o app inteiro sai da tela (nenhuma activity visivel) e volta. Com a opcao "Continuar
 * rodando a VM em segundo plano" desligada, a VM pausa ao sair e continua ao voltar; uma VM pausada
 * pelo usuario nunca e retomada aqui. Com a opcao ligada, quem mantem a VM viva e o VmService.
 */
public final class MultiVmApp extends Application {

    private static Context ctx;
    private int started;

    static Context context() {
        return ctx;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        ctx = getApplicationContext();
        registerActivityLifecycleCallbacks(new ActivityLifecycleCallbacks() {
            @Override
            public void onActivityStarted(Activity a) {
                if (started++ == 0) onForeground();
            }

            @Override
            public void onActivityStopped(Activity a) {
                /* rotacao recria a activity: so conta como saida se nao for troca de configuracao */
                if (--started == 0 && !a.isChangingConfigurations()) onBackground();
            }

            @Override
            public void onActivityCreated(Activity a, Bundle b) {}

            @Override
            public void onActivityResumed(Activity a) {}

            @Override
            public void onActivityPaused(Activity a) {}

            @Override
            public void onActivitySaveInstanceState(Activity a, Bundle b) {}

            @Override
            public void onActivityDestroyed(Activity a) {}
        });
    }

    private void onBackground() {
        VirtualMachine vm = VmHolder.vm;
        if (vm == null || AppSettings.runInBackground(this)) return;
        try {
            if (vm.getState() == VirtualMachine.State.RUNNING && !vm.isPaused()) {
                vm.pause();
                VmHolder.autoPaused = true;
            }
        } catch (IllegalStateException ignored) {
            /* VM fechando */
        }
    }

    private void onForeground() {
        VirtualMachine vm = VmHolder.vm;
        if (vm != null && VmHolder.autoPaused) {
            VmHolder.autoPaused = false;
            try {
                if (vm.isPaused()) vm.resume();
            } catch (IllegalStateException ignored) {
                /* VM fechando */
            }
        }
        /* a opcao pode ter mudado com a VM rodando; o servico so pode iniciar com o app visivel */
        VmService.sync(this);
    }
}
