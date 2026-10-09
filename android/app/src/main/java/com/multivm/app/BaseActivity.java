package com.multivm.app;

import android.app.Activity;
import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.view.View;

/** Activity com o tema escolhido em Configuracoes; recria-se se o tema mudar. */
abstract class BaseActivity extends Activity {

    private int themeAtCreate;

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(AppSettings.wrap(base));
    }

    @Override
    protected void onCreate(Bundle saved) {
        themeAtCreate = AppSettings.theme(this);
        super.onCreate(saved);
        if (lightSystemBars() && AppSettings.isLight(this) && Build.VERSION.SDK_INT >= 26) {
            /* icones escuros na barra de navegacao clara (a de status vem do tema) */
            View d = getWindow().getDecorView();
            d.setSystemUiVisibility(d.getSystemUiVisibility() | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (AppSettings.theme(this) != themeAtCreate) recreate();
    }

    /** false: a tela mantem as barras do sistema escuras (tela da VM). */
    protected boolean lightSystemBars() {
        return true;
    }
}
