package com.multivm.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

import com.multivm.core.FramebufferInfo;
import com.multivm.core.VirtualMachine;

/**
 * Mostra o framebuffer da VM e converte toque/teclado em eventos do convidado.
 *
 * <p>Toque funciona como touchpad (o mouse PS/2 e relativo): arrastar move o
 * ponteiro, toque = clique esquerdo, toque com dois dedos = clique direito,
 * arrastar com dois dedos = roda, segurar parado e arrastar = arrastar com o
 * botao esquerdo. Um mouse fisico tambem funciona.
 *
 * <p>Zoom: pinca com dois dedos amplia (ate 5x) em volta dos dedos; enquanto os dois dedos
 * continuam na tela, move-los arrasta a imagem ampliada. Tres dedos arrastam a imagem a qualquer
 * momento. {@link #resetZoom()} volta ao tamanho normal.
 */
public class ScreenView extends View {

    /** Recebe a entrada destinada ao convidado. */
    interface InputSink {
        /** tecla de hardware (codigo KeyEvent.KEYCODE_*) */
        boolean onHardKey(int keyCode, boolean down);
        /** caractere digitado no teclado virtual */
        void onChar(char c);
        void onPointer(int dx, int dy, int wheel, int buttons);
    }

    private InputSink sink;
    private Bitmap bitmap;
    private int lastGen = -1;
    private final Rect dst = new Rect();
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float sensitivity = 1f;
    /* Configuracoes > Tela da VM e Mouse e toque */
    private int scaleMode = AppSettings.SCALE_FIT;
    private boolean tapClick = true, twoFingerRight = true, longPressDrag = true, leftHanded, haptics = true;
    private float wheelFactor = 1f;

    /* touchpad */
    private final int slop;
    private float lastX, lastY, accX, accY, accWheel;
    private float downX, downY;
    private long downTime;
    private int maxPointers;
    private boolean moved, dragging;
    private int buttons;
    /* gesto de dois dedos: 0 = indefinido, 1 = roda do mouse, 2 = zoom (pinca) e arrastar a imagem */
    private int twoMode;
    private float startSpan, lastSpan, startFx, startFy;

    /* zoom: fator sobre a escala escolhida e deslocamento (pixels da tela) do centro da imagem */
    static final float ZOOM_MAX = 5f;
    private float zoom = 1f, panX, panY;
    /* retangulo da imagem sem zoom (calculado no onDraw) */
    private float baseX, baseY, baseW, baseH;
    /* tamanho da area sem o teclado virtual: com o teclado aberto a view encolhe so na altura, e a
     * escala continua a de antes (a imagem passa por baixo do teclado e rola) em vez de diminuir */
    private int refW, refH;
    private boolean imeShrunk;
    /* ultima posicao conhecida do ponteiro do convidado (para a imagem acompanhar) */
    private int lastCurX = Integer.MIN_VALUE, lastCurY;
    /** avisado quando o zoom muda (para o rotulo do botao) */
    interface ZoomListener { void onZoom(float zoom); }
    private ZoomListener zoomListener;
    private final Runnable longPress = () -> {
        if (!moved && maxPointers == 1) {
            dragging = true;
            setButtons(primary());
            if (haptics) performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
        }
    };

    /* mouse fisico */
    private boolean hoverValid;
    private float hoverX, hoverY;

    public ScreenView(Context context, AttributeSet attrs) {
        super(context, attrs);
        setFocusable(true);
        setFocusableInTouchMode(true);
        slop = ViewConfiguration.get(context).getScaledTouchSlop();
        textPaint.setColor(0xff808080);
        textPaint.setTextSize(14 * getResources().getDisplayMetrics().scaledDensity);
        textPaint.setTextAlign(Paint.Align.CENTER);
    }

    void setInputSink(InputSink s) { sink = s; }

    void setSensitivity(float s) { sensitivity = s; }

    /** Aplica escala, suavizacao e gestos de Configuracoes. */
    void applySettings(AppSettings s) {
        if (s.scale != scaleMode) panX = panY = 0;
        scaleMode = s.scale;
        paint.setFilterBitmap(s.smooth);
        tapClick = s.tapClick;
        twoFingerRight = s.twoFingerRight;
        longPressDrag = s.longPressDrag;
        leftHanded = s.leftHanded;
        haptics = s.haptics;
        wheelFactor = (s.invertScroll ? -1f : 1f) * s.scrollSpeed;
        invalidate();
    }

    /** botao do toque simples (o direito para canhotos) */
    private int primary() {
        return leftHanded ? VirtualMachine.BUTTON_RIGHT : VirtualMachine.BUTTON_LEFT;
    }

    private int secondary() {
        return leftHanded ? VirtualMachine.BUTTON_LEFT : VirtualMachine.BUTTON_RIGHT;
    }

    /** Copia o framebuffer se mudou. Chamado na thread de UI. */
    void refresh(VirtualMachine vm) {
        int gen = vm.getFramebufferGeneration();
        if (gen == lastGen && bitmap != null) return;
        int r = bitmap != null ? vm.updateBitmap(bitmap) : -1;
        if (r < 0) {
            /* primeiro quadro ou o convidado trocou de resolucao */
            FramebufferInfo fi = vm.getFramebufferInfo();
            if (fi == null || fi.width <= 0 || fi.height <= 0) return;
            if (bitmap == null || bitmap.getWidth() != fi.width || bitmap.getHeight() != fi.height)
                bitmap = Bitmap.createBitmap(fi.width, fi.height, Bitmap.Config.ARGB_8888);
            r = vm.updateBitmap(bitmap);
            if (r < 0) return;
            invalidate();
        } else if (r > 0) {
            invalidate();
        }
        lastGen = gen;
        followCursor(vm);
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        if (refH > 0 && w == refW && h < refH * 0.85f) {
            imeShrunk = true; /* teclado virtual aberto: mantem a escala */
        } else {
            refW = w;
            refH = h;
            imeShrunk = false;
        }
        lastCurX = Integer.MIN_VALUE; /* reposiciona pelo ponteiro no proximo quadro */
    }

    /** Leva a imagem ate o ponteiro do convidado quando ela e maior que a tela (zoom/teclado/preencher). */
    private void followCursor(VirtualMachine vm) {
        int[] p;
        try {
            p = vm.getCursorPosition();
        } catch (RuntimeException e) {
            return;
        }
        if (p == null || bitmap == null || dst.width() <= 0) return;
        if (p[0] == lastCurX && p[1] == lastCurY) return;
        lastCurX = p[0];
        lastCurY = p[1];
        int vw = getWidth(), vh = getHeight();
        if (dst.width() <= vw && dst.height() <= vh) return;
        float vx = dst.left + p[0] * (float) dst.width() / bitmap.getWidth();
        float vy = dst.top + p[1] * (float) dst.height() / bitmap.getHeight();
        float mx = vw * 0.15f, my = vh * 0.2f, dx = 0, dy = 0;
        if (vx < mx) dx = mx - vx;
        else if (vx > vw - mx) dx = vw - mx - vx;
        if (vy < my) dy = my - vy;
        else if (vy > vh - my) dy = vh - my - vy;
        if (dx != 0 || dy != 0) panBy(dx, dy);
    }

    /** Resolucao atual do convidado (para o status), ou null. */
    String resolution() {
        return bitmap == null ? null : bitmap.getWidth() + "x" + bitmap.getHeight();
    }

    void setZoomListener(ZoomListener l) { zoomListener = l; }

    float zoom() { return zoom; }

    /** Volta ao tamanho normal (sem zoom). */
    void resetZoom() {
        setZoom(1f, getWidth() / 2f, getHeight() / 2f);
        panX = panY = 0;
        invalidate();
    }

    /** Zoom absoluto em volta do ponto (fx, fy) da tela. */
    void setZoom(float z, float fx, float fy) {
        z = Math.max(1f, Math.min(ZOOM_MAX, z));
        if (baseW > 0 && baseH > 0) {
            /* o ponto da imagem sob (fx, fy) continua sob ele */
            float w = baseW * zoom, h = baseH * zoom;
            float left = baseX + baseW / 2f + panX - w / 2f, top = baseY + baseH / 2f + panY - h / 2f;
            float u = (fx - left) / w, v = (fy - top) / h;
            float nw = baseW * z, nh = baseH * z;
            float nl = fx - u * nw, nt = fy - v * nh;
            panX = nl + nw / 2f - (baseX + baseW / 2f);
            panY = nt + nh / 2f - (baseY + baseH / 2f);
        }
        boolean changed = z != zoom;
        zoom = z;
        if (z == 1f && scaleMode != AppSettings.SCALE_FILL) panX = panY = 0;
        invalidate();
        if (changed && zoomListener != null) zoomListener.onZoom(z);
    }

    private void panBy(float dx, float dy) {
        panX += dx;
        panY += dy;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas c) {
        c.drawColor(0xff000000);
        if (bitmap == null) {
            c.drawText("sem imagem de vídeo", getWidth() / 2f, getHeight() / 2f, textPaint);
            return;
        }
        int vw = getWidth(), vh = getHeight();
        /* escala pela area sem o teclado virtual */
        int rw = imeShrunk ? refW : vw, rh = imeShrunk ? refH : vh;
        int bw = bitmap.getWidth(), bh = bitmap.getHeight();
        float fit = Math.min((float) rw / bw, (float) rh / bh);
        float w, h;
        switch (scaleMode) {
            case AppSettings.SCALE_STRETCH:
                w = rw;
                h = rh;
                break;
            case AppSettings.SCALE_FILL: { /* cobre a tela toda; o que sobra fica fora (arrastar com 3 dedos) */
                float cover = Math.max((float) rw / bw, (float) rh / bh);
                w = bw * cover;
                h = bh * cover;
                break;
            }
            case AppSettings.SCALE_INTEGER: {
                float s = fit >= 1f ? (float) Math.floor(fit) : fit; /* menor que a tela: so reduz */
                w = Math.round(bw * s);
                h = Math.round(bh * s);
                break;
            }
            case AppSettings.SCALE_ORIGINAL:
                w = bw;
                h = bh;
                break;
            default:
                w = Math.round(bw * fit);
                h = Math.round(bh * fit);
        }
        baseX = (rw - w) / 2f;
        baseY = (rh - h) / 2f;
        baseW = w;
        baseH = h;
        /* zoom e deslocamento; a imagem nao sai da tela: maior que ela, encosta nas bordas; menor, centraliza */
        float zw = w * zoom, zh = h * zoom;
        float left = baseX + w / 2f + panX - zw / 2f, top = baseY + h / 2f + panY - zh / 2f;
        left = zw > vw ? Math.max(vw - zw, Math.min(0, left)) : (vw - zw) / 2f;
        top = zh > vh ? Math.max(vh - zh, Math.min(0, top)) : (vh - zh) / 2f;
        panX = left + zw / 2f - (baseX + w / 2f);
        panY = top + zh / 2f - (baseY + h / 2f);
        dst.set(Math.round(left), Math.round(top), Math.round(left + zw), Math.round(top + zh));
        c.drawBitmap(bitmap, null, dst, paint);
    }

    /** pixels do convidado por pixel da tela (horizontal e vertical: diferem ao esticar) */
    private float guestScale() {
        if (bitmap == null || dst.width() == 0) return 1f;
        return (float) bitmap.getWidth() / dst.width();
    }

    private float guestScaleY() {
        if (bitmap == null || dst.height() == 0) return 1f;
        return (float) bitmap.getHeight() / dst.height();
    }

    private void move(float dxView, float dyView, float wheel) {
        accX += dxView * guestScale() * sensitivity;
        accY += dyView * guestScaleY() * sensitivity;
        accWheel += wheel * wheelFactor;
        int dx = (int) accX, dy = (int) accY, dw = (int) accWheel;
        if (dx == 0 && dy == 0 && dw == 0) return;
        accX -= dx;
        accY -= dy;
        accWheel -= dw;
        if (sink != null) sink.onPointer(dx, dy, dw, buttons);
    }

    /** Movimento vindo do trackpad virtual (pixels de tela, com a mesma escala e sensibilidade). */
    void trackpadMove(float dx, float dy, float wheel) {
        move(dx, dy, wheel);
    }

    /** Botoes vindos do trackpad virtual (mascara VirtualMachine.BUTTON_*). */
    void trackpadButtons(int b) {
        setButtons(b);
    }

    int currentButtons() { return buttons; }

    private void setButtons(int b) {
        buttons = b;
        if (sink != null) sink.onPointer(0, 0, 0, b);
    }

    private void click(int b) {
        setButtons(b);
        postDelayed(() -> setButtons(0), 40);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (e.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) return mouseEvent(e);
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                requestFocus();
                lastX = downX = e.getX();
                lastY = downY = e.getY();
                downTime = e.getEventTime();
                maxPointers = 1;
                moved = dragging = false;
                twoMode = 0;
                startSpan = 0;
                if (longPressDrag) postDelayed(longPress, ViewConfiguration.getLongPressTimeout());
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
                maxPointers = Math.max(maxPointers, e.getPointerCount());
                removeCallbacks(longPress);
                lastX = avgX(e);
                lastY = avgY(e);
                lastSpan = span(e, -1);
                if (e.getPointerCount() == 2 && startSpan == 0) {
                    startSpan = lastSpan;
                    startFx = lastX;
                    startFy = lastY;
                }
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                /* recalcula a referencia sem o dedo que saiu */
                lastX = avgX(e, e.getActionIndex());
                lastY = avgY(e, e.getActionIndex());
                lastSpan = span(e, e.getActionIndex());
                return true;
            case MotionEvent.ACTION_MOVE: {
                float x = avgX(e), y = avgY(e);
                int n = e.getPointerCount();
                if (!moved && Math.hypot(e.getX() - downX, e.getY() - downY) > slop) {
                    moved = true;
                    removeCallbacks(longPress);
                }
                if (n >= 3) {
                    /* tres dedos: arrasta a imagem (com zoom ou no modo preencher) */
                    moved = true;
                    panBy(x - lastX, y - lastY);
                } else if (n == 2) {
                    float sp = span(e, -1);
                    /* decide o gesto: os dedos se afastando/aproximando = zoom; andando juntos = roda */
                    if (twoMode == 0 && startSpan > 0) {
                        if (Math.abs(sp / startSpan - 1f) > 0.15f) twoMode = 2;
                        else if (Math.hypot(x - startFx, y - startFy) > slop * 2) twoMode = 1;
                    }
                    if (twoMode == 2) {
                        moved = true;
                        if (lastSpan > 0 && sp > 0) setZoom(zoom * sp / lastSpan, x, y);
                        panBy(x - lastX, y - lastY);
                    } else if (twoMode == 1) {
                        moved = true;
                        move(0, 0, (lastY - y) / (slop * 2f));
                    }
                    lastSpan = sp;
                } else if (moved || dragging) {
                    move(x - lastX, y - lastY, 0);
                }
                lastX = x;
                lastY = y;
                return true;
            }
            case MotionEvent.ACTION_UP:
                removeCallbacks(longPress);
                if (dragging) {
                    setButtons(0);
                } else if (!moved && e.getEventTime() - downTime < ViewConfiguration.getLongPressTimeout()) {
                    if (maxPointers >= 2) {
                        if (twoFingerRight) click(secondary());
                    } else if (tapClick) {
                        click(primary());
                    }
                }
                dragging = false;
                return true;
            case MotionEvent.ACTION_CANCEL:
                removeCallbacks(longPress);
                if (buttons != 0) setButtons(0);
                dragging = false;
                return true;
        }
        return super.onTouchEvent(e);
    }

    /** distancia media dos dedos ao centro (sem o dedo skip) */
    private static float span(MotionEvent e, int skip) {
        float cx = avgX(e, skip), cy = avgY(e, skip), s = 0;
        int n = 0;
        for (int i = 0; i < e.getPointerCount(); i++)
            if (i != skip) {
                s += (float) Math.hypot(e.getX(i) - cx, e.getY(i) - cy);
                n++;
            }
        return n < 2 ? 0 : s / n;
    }

    private static float avgX(MotionEvent e) { return avgX(e, -1); }

    private static float avgY(MotionEvent e) { return avgY(e, -1); }

    private static float avgX(MotionEvent e, int skip) {
        float s = 0;
        int n = 0;
        for (int i = 0; i < e.getPointerCount(); i++) if (i != skip) { s += e.getX(i); n++; }
        return n == 0 ? e.getX() : s / n;
    }

    private static float avgY(MotionEvent e, int skip) {
        float s = 0;
        int n = 0;
        for (int i = 0; i < e.getPointerCount(); i++) if (i != skip) { s += e.getY(i); n++; }
        return n == 0 ? e.getY() : s / n;
    }

    /* ---- mouse fisico (USB/Bluetooth) ---- */

    private static int mouseButtons(MotionEvent e) {
        int s = e.getButtonState(), b = 0;
        if ((s & MotionEvent.BUTTON_PRIMARY) != 0) b |= VirtualMachine.BUTTON_LEFT;
        if ((s & MotionEvent.BUTTON_SECONDARY) != 0) b |= VirtualMachine.BUTTON_RIGHT;
        if ((s & MotionEvent.BUTTON_TERTIARY) != 0) b |= VirtualMachine.BUTTON_MIDDLE;
        return b;
    }

    private boolean mouseEvent(MotionEvent e) {
        float x = e.getX(), y = e.getY();
        if (hoverValid) {
            accX += (x - hoverX) * guestScale();
            accY += (y - hoverY) * guestScaleY();
        }
        hoverX = x;
        hoverY = y;
        hoverValid = true;
        int dx = (int) accX, dy = (int) accY;
        accX -= dx;
        accY -= dy;
        buttons = mouseButtons(e);
        if (sink != null) sink.onPointer(dx, dy, 0, buttons);
        return true;
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent e) {
        if (e.isFromSource(InputDevice.SOURCE_CLASS_POINTER)) {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_HOVER_MOVE:
                    return mouseEvent(e);
                case MotionEvent.ACTION_HOVER_EXIT:
                    hoverValid = false;
                    return true;
                case MotionEvent.ACTION_SCROLL: {
                    accWheel += -e.getAxisValue(MotionEvent.AXIS_VSCROLL) * wheelFactor;
                    int w = (int) accWheel;
                    accWheel -= w;
                    if (w != 0 && sink != null) sink.onPointer(0, 0, w, buttons);
                    return true;
                }
            }
        }
        return super.onGenericMotionEvent(e);
    }

    /* ---- teclado ---- */

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) return super.onKeyDown(keyCode, event);
        if (sink != null && sink.onHardKey(keyCode, true)) return true;
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) return super.onKeyUp(keyCode, event);
        if (sink != null && sink.onHardKey(keyCode, false)) return true;
        return super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean onCheckIsTextEditor() { return true; }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo out) {
        /* senha visivel: sem sugestoes nem composicao, cada letra chega na hora */
        out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        out.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN
                | EditorInfo.IME_ACTION_NONE;
        return new BaseInputConnection(this, false) {
            /* texto em composicao so e enviado quando o teclado o confirma */
            private CharSequence composing = "";

            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                composing = "";
                if (sink != null) for (int i = 0; i < text.length(); i++) sink.onChar(text.charAt(i));
                return true;
            }

            @Override
            public boolean setComposingText(CharSequence text, int newCursorPosition) {
                composing = text;
                return true;
            }

            @Override
            public boolean finishComposingText() {
                if (composing.length() > 0) commitText(composing, 1);
                return true;
            }

            @Override
            public boolean deleteSurroundingText(int before, int after) {
                for (int i = 0; i < before; i++) {
                    if (sink != null) {
                        sink.onHardKey(KeyEvent.KEYCODE_DEL, true);
                        sink.onHardKey(KeyEvent.KEYCODE_DEL, false);
                    }
                }
                return true;
            }
        };
    }
}
