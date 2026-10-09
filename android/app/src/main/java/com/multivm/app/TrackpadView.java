package com.multivm.app;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.multivm.core.VirtualMachine;

/**
 * Trackpad virtual flutuante: alca de arrasto (move o painel para qualquer lugar da tela),
 * superficie de toque (movimento relativo; toque rapido = clique esquerdo; dois dedos = rolagem)
 * e os botoes esquerdo, meio e direito, pressionados enquanto o dedo estiver neles.
 */
@SuppressLint({"ViewConstructor", "ClickableViewAccessibility"})
final class TrackpadView extends LinearLayout {
    private static final int[] BUTTONS = {VirtualMachine.BUTTON_LEFT, VirtualMachine.BUTTON_MIDDLE,
            VirtualMachine.BUTTON_RIGHT};
    private static final String[] LABELS = {"Esquerdo", "Meio", "Direito"};

    private final ScreenView screen;
    private final SharedPreferences prefs;
    private final int slop;

    /* superficie */
    private float lastX, lastY;
    private long downTime;
    private float moved;
    private boolean twoFingers;
    /* botoes presos pelos botoes do painel */
    private int held;

    TrackpadView(Context ctx, ScreenView screen, Runnable onClose) {
        super(ctx);
        this.screen = screen;
        prefs = ctx.getSharedPreferences("trackpad", Context.MODE_PRIVATE);
        slop = ViewConfiguration.get(ctx).getScaledTouchSlop();
        setOrientation(VERTICAL);
        setElevation(dp(8));
        setPadding(dp(6), dp(2), dp(6), dp(6));
        setBackground(round(getContext().getColor(R.color.trackpad_bg), 18, getContext().getColor(R.color.outline)));

        /* alca: arrasta o painel; X fecha */
        LinearLayout bar = new LinearLayout(ctx);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        TextView grip = new TextView(ctx);
        grip.setText("⋮⋮  Trackpad");
        grip.setTextColor(getContext().getColor(R.color.text2));
        grip.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        grip.setPadding(dp(6), 0, 0, 0);
        bar.addView(grip, new LayoutParams(0, dp(30), 1));
        grip.setGravity(Gravity.CENTER_VERTICAL);
        ImageView close = new ImageView(ctx);
        close.setImageResource(R.drawable.ic_close);
        close.setImageTintList(android.content.res.ColorStateList.valueOf(getContext().getColor(R.color.text2)));
        close.setPadding(dp(5), dp(5), dp(5), dp(5));
        close.setContentDescription("Fechar trackpad");
        close.setOnClickListener(v -> onClose.run());
        bar.addView(close, new LayoutParams(dp(30), dp(30)));
        addView(bar, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
        bar.setOnTouchListener(new Dragger());

        /* superficie de toque */
        View pad = new View(ctx);
        pad.setBackground(round(getContext().getColor(R.color.trackpad_pad), 12, 0));
        pad.setOnTouchListener((v, e) -> padTouch(e));
        pad.setContentDescription("Superfície do trackpad");
        addView(pad, new LayoutParams(LayoutParams.MATCH_PARENT, dp(130)));

        /* botoes do mouse */
        LinearLayout row = new LinearLayout(ctx);
        LayoutParams rl = new LayoutParams(LayoutParams.MATCH_PARENT, dp(46));
        rl.topMargin = dp(6);
        addView(row, rl);
        for (int i = 0; i < BUTTONS.length; i++) {
            final int bit = BUTTONS[i];
            TextView b = new TextView(ctx);
            b.setText(LABELS[i]);
            b.setGravity(Gravity.CENTER);
            b.setTextColor(getContext().getColor(R.color.text));
            b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            b.setBackground(round(getContext().getColor(R.color.surface2), 10, 0));
            b.setOnTouchListener((v, e) -> buttonTouch(v, e, bit));
            LayoutParams lp = new LayoutParams(0, LayoutParams.MATCH_PARENT, i == 1 ? 0.8f : 1f);
            if (i > 0) lp.setMarginStart(dp(5));
            row.addView(b, lp);
        }
    }

    /** Coloca o painel na posicao lembrada (ou no canto inferior direito). */
    void place(FrameLayout parent) {
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(dp(250), ViewGroup.LayoutParams.WRAP_CONTENT);
        parent.addView(this, lp);
        post(() -> {
            float fx = prefs.getFloat("x", 1f), fy = prefs.getFloat("y", 1f);
            moveTo(fx * (parent.getWidth() - getWidth()), fy * (parent.getHeight() - getHeight()));
        });
    }

    /** A tela girou: mantem a posicao relativa dentro dos limites. */
    void keepInside() {
        post(() -> moveTo(getX(), getY()));
    }

    private void moveTo(float x, float y) {
        View p = (View) getParent();
        if (p == null) return;
        float maxX = Math.max(0, p.getWidth() - getWidth()), maxY = Math.max(0, p.getHeight() - getHeight());
        setX(Math.max(0, Math.min(maxX, x)));
        setY(Math.max(0, Math.min(maxY, y)));
    }

    private void savePosition() {
        View p = (View) getParent();
        if (p == null) return;
        float rx = p.getWidth() > getWidth() ? getX() / (p.getWidth() - getWidth()) : 0;
        float ry = p.getHeight() > getHeight() ? getY() / (p.getHeight() - getHeight()) : 0;
        prefs.edit().putFloat("x", rx).putFloat("y", ry).apply();
    }

    /** Solta os botoes presos (ao fechar o painel). */
    void release() {
        if (held != 0) {
            held = 0;
            screen.trackpadButtons(0);
        }
    }

    private final class Dragger implements OnTouchListener {
        float offX, offY;

        @SuppressLint("ClickableViewAccessibility")
        @Override
        public boolean onTouch(View v, MotionEvent e) {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    offX = e.getRawX() - getX();
                    offY = e.getRawY() - getY();
                    return true;
                case MotionEvent.ACTION_MOVE:
                    moveTo(e.getRawX() - offX, e.getRawY() - offY);
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    savePosition();
                    return true;
                default:
                    return false;
            }
        }
    }

    private boolean padTouch(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                lastX = e.getX();
                lastY = e.getY();
                downTime = e.getEventTime();
                moved = 0;
                twoFingers = false;
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
                twoFingers = true;
                lastX = avgX(e);
                lastY = avgY(e);
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                /* volta para um dedo sem pular: recomeca da posicao do dedo que ficou */
                int keep = e.getActionIndex() == 0 ? 1 : 0;
                lastX = e.getX(keep);
                lastY = e.getY(keep);
                return true;
            case MotionEvent.ACTION_MOVE: {
                if (twoFingers && e.getPointerCount() >= 2) {
                    float y = avgY(e), dy = y - lastY;
                    lastX = avgX(e);
                    lastY = y;
                    moved += Math.abs(dy);
                    screen.trackpadMove(0, 0, -dy / dp(14));
                    return true;
                }
                float dx = e.getX() - lastX, dy = e.getY() - lastY;
                lastX = e.getX();
                lastY = e.getY();
                moved += Math.abs(dx) + Math.abs(dy);
                /* o trackpad e pequeno: acelera um pouco em relacao ao toque na tela */
                screen.trackpadMove(dx * 1.6f, dy * 1.6f, 0);
                return true;
            }
            case MotionEvent.ACTION_UP:
                if (!twoFingers && moved < slop && e.getEventTime() - downTime < 250) {
                    int base = held;
                    screen.trackpadButtons(base | VirtualMachine.BUTTON_LEFT);
                    screen.trackpadButtons(base);
                }
                return true;
            default:
                return true;
        }
    }

    private boolean buttonTouch(View v, MotionEvent e, int bit) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                held |= bit;
                v.setPressed(true);
                v.setAlpha(0.6f);
                v.performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
                screen.trackpadButtons(held);
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                held &= ~bit;
                v.setPressed(false);
                v.setAlpha(1f);
                screen.trackpadButtons(held);
                if (e.getActionMasked() == MotionEvent.ACTION_UP) v.performClick();
                return true;
            default:
                return true;
        }
    }

    private static float avgX(MotionEvent e) {
        return (e.getX(0) + e.getX(1)) / 2;
    }

    private static float avgY(MotionEvent e) {
        return (e.getY(0) + e.getY(1)) / 2;
    }

    private GradientDrawable round(int color, int radiusDp, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(radiusDp));
        if (stroke != 0) g.setStroke(dp(1), stroke);
        return g;
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }
}
