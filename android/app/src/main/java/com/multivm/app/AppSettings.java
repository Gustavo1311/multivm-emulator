package com.multivm.app;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;

/**
 * Configuracoes gerais do app (menu Configuracoes), nas preferencias "app": tema, tela da VM,
 * mouse/toque e discos novos. Valem para todas as VMs.
 */
final class AppSettings {

    private static final String PREFS = "app";

    static final int THEME_DARK = 0, THEME_LIGHT = 1, THEME_SYSTEM = 2;
    static final String[] THEME_NAMES = {"Escuro", "Claro", "Seguir o sistema"};

    /** Escala da imagem da VM na tela do aparelho. */
    static final int SCALE_FIT = 0, SCALE_STRETCH = 1, SCALE_INTEGER = 2, SCALE_ORIGINAL = 3;
    static final String[] SCALE_NAMES = {"Ajustar à tela (mantém a proporção)", "Esticar (preenche a tela toda)",
            "Múltiplo inteiro (pixels nítidos)", "Tamanho original (1 pixel da VM = 1 pixel da tela)"};

    static final int[] FPS = {30, 60};
    static final String[] FPS_NAMES = {"30 quadros/s (economiza bateria)", "60 quadros/s (mais fluido)"};

    static final int ORIENT_AUTO = 0, ORIENT_LANDSCAPE = 1, ORIENT_PORTRAIT = 2;
    static final String[] ORIENT_NAMES = {"Automática (gira com o aparelho)", "Paisagem", "Retrato"};

    /** Resolucoes oferecidas para a tela grafica (framebuffer) das VMs com kernel Linux. */
    static final String[] RESOLUTIONS = {"640x480", "800x600", "1024x768", "1280x720", "1280x800", "1280x1024",
            "1366x768", "1440x900", "1600x900", "1920x1080"};

    static final int DISK_ASK = 0, DISK_APP = 1;
    static final String[] DISK_NAMES = {"Perguntar a pasta a cada disco novo", "Sempre na pasta do app"};

    int theme = THEME_DARK;
    /* tela da VM */
    int scale = SCALE_FIT;
    boolean smooth = true;
    int fps = 0;
    boolean keepScreenOn = true;
    int orientation = ORIENT_AUTO;
    boolean startFullscreen;
    String defaultResolution = "800x600";
    /* mouse e toque */
    float mouseSpeed = 1f;
    boolean tapClick = true, twoFingerRight = true, longPressDrag = true;
    boolean invertScroll, leftHanded;
    float scrollSpeed = 1f;
    boolean haptics = true;
    boolean trackpadOnStart;
    /* teclado */
    boolean showKeys;
    /* discos */
    int diskLocation = DISK_ASK;

    private AppSettings() {}

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static AppSettings load(Context c) {
        SharedPreferences p = prefs(c);
        AppSettings s = new AppSettings();
        s.theme = clamp(p.getInt("theme", THEME_DARK), THEME_NAMES.length);
        s.scale = clamp(p.getInt("scale", SCALE_FIT), SCALE_NAMES.length);
        s.smooth = p.getBoolean("smooth", true);
        s.fps = clamp(p.getInt("fps", 0), FPS.length);
        s.keepScreenOn = p.getBoolean("keepScreenOn", true);
        s.orientation = clamp(p.getInt("orientation", ORIENT_AUTO), ORIENT_NAMES.length);
        s.startFullscreen = p.getBoolean("startFullscreen", false);
        s.defaultResolution = p.getString("defaultResolution", "800x600");
        s.mouseSpeed = p.getFloat("mouseSpeed", 1f);
        s.tapClick = p.getBoolean("tapClick", true);
        s.twoFingerRight = p.getBoolean("twoFingerRight", true);
        s.longPressDrag = p.getBoolean("longPressDrag", true);
        s.invertScroll = p.getBoolean("invertScroll", false);
        s.leftHanded = p.getBoolean("leftHanded", false);
        s.scrollSpeed = p.getFloat("scrollSpeed", 1f);
        s.haptics = p.getBoolean("haptics", true);
        s.trackpadOnStart = p.getBoolean("trackpadOnStart", false);
        /* a barra de teclas ja era lembrada em "ui" pela tela da VM */
        s.showKeys = c.getSharedPreferences("ui", Context.MODE_PRIVATE).getBoolean("showKeys", false);
        s.diskLocation = clamp(p.getInt("diskLocation", DISK_ASK), DISK_NAMES.length);
        return s;
    }

    void save(Context c) {
        prefs(c).edit()
                .putInt("theme", theme)
                .putInt("scale", scale)
                .putBoolean("smooth", smooth)
                .putInt("fps", fps)
                .putBoolean("keepScreenOn", keepScreenOn)
                .putInt("orientation", orientation)
                .putBoolean("startFullscreen", startFullscreen)
                .putString("defaultResolution", defaultResolution)
                .putFloat("mouseSpeed", mouseSpeed)
                .putBoolean("tapClick", tapClick)
                .putBoolean("twoFingerRight", twoFingerRight)
                .putBoolean("longPressDrag", longPressDrag)
                .putBoolean("invertScroll", invertScroll)
                .putBoolean("leftHanded", leftHanded)
                .putFloat("scrollSpeed", scrollSpeed)
                .putBoolean("haptics", haptics)
                .putBoolean("trackpadOnStart", trackpadOnStart)
                .putInt("diskLocation", diskLocation)
                .apply();
        c.getSharedPreferences("ui", Context.MODE_PRIVATE).edit().putBoolean("showKeys", showKeys).apply();
    }

    /** Volta tudo ao padrao (o tema inclusive). */
    static void reset(Context c) {
        prefs(c).edit().clear().apply();
        c.getSharedPreferences("ui", Context.MODE_PRIVATE).edit().remove("showKeys").apply();
    }

    private static int clamp(int v, int n) { return v < 0 || v >= n ? 0 : v; }

    static int theme(Context c) {
        return clamp(prefs(c).getInt("theme", THEME_DARK), THEME_NAMES.length);
    }

    /**
     * Contexto com o tema escolhido: forca o modo noturno (cores de values/) ou o diurno
     * (values-notnight/). "Seguir o sistema" deixa a configuracao do aparelho.
     */
    static Context wrap(Context base) {
        int t = theme(base);
        if (t == THEME_SYSTEM) return base;
        Configuration c = new Configuration(base.getResources().getConfiguration());
        c.uiMode = (c.uiMode & ~Configuration.UI_MODE_NIGHT_MASK)
                | (t == THEME_LIGHT ? Configuration.UI_MODE_NIGHT_NO : Configuration.UI_MODE_NIGHT_YES);
        return base.createConfigurationContext(c);
    }

    /** O contexto (ja embrulhado por wrap) esta no tema claro. */
    static boolean isLight(Context c) {
        return (c.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                == Configuration.UI_MODE_NIGHT_NO;
    }
}
