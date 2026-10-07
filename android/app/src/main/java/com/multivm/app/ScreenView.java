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

    /* touchpad */
    private final int slop;
    private float lastX, lastY, accX, accY, accWheel;
    private float downX, downY;
    private long downTime;
    private int maxPointers;
    private boolean moved, dragging;
    private int buttons;
    private final Runnable longPress = () -> {
        if (!moved && maxPointers == 1) {
            dragging = true;
            setButtons(VirtualMachine.BUTTON_LEFT);
            performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
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

    /** Copia o framebuffer se mudou. Chamado na thread de UI. */
    void refresh(VirtualMachine vm) {
        int gen = vm.getFramebufferGeneration();
        if (gen == lastGen && bitmap != null) return;
        FramebufferInfo fi = vm.getFramebufferInfo();
        if (fi == null || fi.width <= 0 || fi.height <= 0) return;
        if (bitmap == null || bitmap.getWidth() != fi.width || bitmap.getHeight() != fi.height) {
            bitmap = Bitmap.createBitmap(fi.width, fi.height, Bitmap.Config.ARGB_8888);
        }
        if (vm.copyFramebuffer(bitmap)) {
            lastGen = gen;
            invalidate();
        }
    }

    /** Resolucao atual do convidado (para o status), ou null. */
    String resolution() {
        return bitmap == null ? null : bitmap.getWidth() + "x" + bitmap.getHeight();
    }

    @Override
    protected void onDraw(Canvas c) {
        c.drawColor(0xff000000);
        if (bitmap == null) {
            c.drawText("sem imagem de vídeo", getWidth() / 2f, getHeight() / 2f, textPaint);
            return;
        }
        int vw = getWidth(), vh = getHeight();
        int bw = bitmap.getWidth(), bh = bitmap.getHeight();
        float s = Math.min((float) vw / bw, (float) vh / bh);
        int w = Math.round(bw * s), h = Math.round(bh * s);
        int x = (vw - w) / 2, y = (vh - h) / 2;
        dst.set(x, y, x + w, y + h);
        c.drawBitmap(bitmap, null, dst, paint);
    }

    /** pixels do convidado por pixel da tela */
    private float guestScale() {
        if (bitmap == null || dst.width() == 0) return 1f;
        return (float) bitmap.getWidth() / dst.width();
    }

    private void move(float dxView, float dyView, float wheel) {
        float k = guestScale() * sensitivity;
        accX += dxView * k;
        accY += dyView * k;
        accWheel += wheel;
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
                postDelayed(longPress, ViewConfiguration.getLongPressTimeout());
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
                maxPointers = Math.max(maxPointers, e.getPointerCount());
                removeCallbacks(longPress);
                lastX = avgX(e);
                lastY = avgY(e);
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                /* recalcula a referencia sem o dedo que saiu */
                lastX = avgX(e, e.getActionIndex());
                lastY = avgY(e, e.getActionIndex());
                return true;
            case MotionEvent.ACTION_MOVE: {
                float x = avgX(e), y = avgY(e);
                if (!moved && Math.hypot(e.getX() - downX, e.getY() - downY) > slop) {
                    moved = true;
                    removeCallbacks(longPress);
                }
                if (moved || dragging) {
                    if (e.getPointerCount() >= 2) move(0, 0, (lastY - y) / (slop * 2f));
                    else move(x - lastX, y - lastY, 0);
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
                    click(maxPointers >= 2 ? VirtualMachine.BUTTON_RIGHT : VirtualMachine.BUTTON_LEFT);
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
            float k = guestScale();
            accX += (x - hoverX) * k;
            accY += (y - hoverY) * k;
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
                    int w = Math.round(-e.getAxisValue(MotionEvent.AXIS_VSCROLL));
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
