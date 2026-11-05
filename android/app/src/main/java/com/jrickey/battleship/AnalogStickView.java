package com.jrickey.battleship;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.view.MotionEvent;
import android.view.View;

/**
 * Virtual analog stick (BattleShipDX). Full-screen transparent view that draws and handles the stick.
 *
 *  - Floating mode (default): the base appears where you first touch on the left side of the screen,
 *    so you never have to hunt for the ring. With "follow" on, the base slides along when you drag
 *    past the ring, so the thumb never runs out of room.
 *  - Fixed mode: the ring stays at the configured position; start touching near it.
 *  - Dead zone and sensitivity are configurable.
 */
public final class AnalogStickView extends View {

    private static final int SDL_AXIS_LEFTX = 0;
    private static final int SDL_AXIS_LEFTY = 1;

    interface EditListener { void onSelect(); void onMoved(); void onDrop(); }

    private final TouchLayoutConfig cfg;
    private final TouchLayoutConfig.Item item;
    private final float density;
    private final Paint basePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ringPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint thumbPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint selPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    boolean editMode = false;
    boolean selected = false;
    EditListener editListener;

    private int activeId = -1;
    private float anchorX, anchorY;   // where the base is drawn while active
    private float touchX, touchY;     // thumb position
    private float dragDx, dragDy;

    public AnalogStickView(Context ctx, TouchLayoutConfig cfg) {
        super(ctx);
        this.cfg = cfg;
        this.item = cfg.items.get(TouchLayoutConfig.STICK);
        density = ctx.getResources().getDisplayMetrics().density;
        ringPaint.setStyle(Paint.Style.STROKE);
        ringPaint.setColor(Color.WHITE);
        ringPaint.setStrokeWidth(2.2f * density);
        selPaint.setStyle(Paint.Style.STROKE);
        selPaint.setColor(Color.rgb(255, 214, 10));
        selPaint.setStrokeWidth(2.5f * density);
        selPaint.setPathEffect(new DashPathEffect(new float[] { 8f * density, 6f * density }, 0));
    }

    private float radius() { return 72f * density * cfg.sizeScale * item.scale; }
    private float homeX() { return item.x * getWidth(); }
    private float homeY() { return item.y * getHeight(); }

    @Override
    protected void onDraw(Canvas c) {
        float r = radius();
        float a = cfg.opacity;
        boolean active = activeId >= 0;
        float bx = active ? anchorX : homeX();
        float by = active ? anchorY : homeY();
        float tx = active ? touchX : bx;
        float ty = active ? touchY : by;
        if (!item.visible && !editMode) return;
        float ga = (item.visible ? 1f : 0.35f) * (active ? 1f : (cfg.floating && !editMode ? 0.55f : 1f));

        basePaint.setShader(new RadialGradient(bx, by, r, Color.argb((int) (90 * a * ga), 255, 255, 255),
                Color.argb((int) (40 * a * ga), 30, 30, 40), Shader.TileMode.CLAMP));
        c.drawCircle(bx, by, r, basePaint);
        ringPaint.setAlpha((int) (255 * Math.min(1f, a + 0.15f) * ga));
        c.drawCircle(bx, by, r, ringPaint);
        // direction ticks
        for (int i = 0; i < 4; i++) {
            double ang = i * Math.PI / 2;
            float x1 = bx + (float) Math.cos(ang) * r * 0.72f, y1 = by + (float) Math.sin(ang) * r * 0.72f;
            float x2 = bx + (float) Math.cos(ang) * r * 0.88f, y2 = by + (float) Math.sin(ang) * r * 0.88f;
            c.drawLine(x1, y1, x2, y2, ringPaint);
        }
        float tr = r * 0.46f;
        thumbPaint.setShader(new RadialGradient(tx, ty - tr * 0.3f, tr * 1.3f,
                Color.argb((int) (255 * a * ga), 120, 170, 255), Color.argb((int) (150 * a * ga), 40, 90, 220), Shader.TileMode.CLAMP));
        c.drawCircle(tx, ty, tr, thumbPaint);
        if (editMode && selected) c.drawCircle(bx, by, r + 4f * density, selPaint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        return editMode ? handleEdit(ev) : handlePlay(ev);
    }

    // ── play ────────────────────────────────────────────────────────────
    private boolean handlePlay(MotionEvent ev) {
        if (!item.visible) return false;
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                if (activeId >= 0) return true;
                int i = ev.getActionIndex();
                float x = ev.getX(i), y = ev.getY(i);
                if (cfg.floating) {
                    if (x > getWidth() * 0.60f) return false;   // right side is for the buttons
                    anchorX = x; anchorY = y;
                } else {
                    if (Math.hypot(x - homeX(), y - homeY()) > radius() * 1.7f) return false;
                    anchorX = homeX(); anchorY = homeY();
                }
                activeId = ev.getPointerId(i);
                update(x, y);
                if (cfg.haptics) performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                int i = ev.findPointerIndex(activeId);
                if (i < 0) return true;
                update(ev.getX(i), ev.getY(i));
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_CANCEL:
                release();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                if (ev.getPointerId(ev.getActionIndex()) != activeId) return true;
                release();
                return true;
            }
            default:
                return false;
        }
    }

    private void update(float x, float y) {
        float r = radius();
        float dx = x - anchorX, dy = y - anchorY;
        float len = (float) Math.hypot(dx, dy);
        if (len > r && cfg.follow && cfg.floating && len > 0f) {
            // slide the base so the finger sits on the ring edge
            float k = (len - r) / len;
            anchorX += dx * k; anchorY += dy * k;
            dx = x - anchorX; dy = y - anchorY; len = (float) Math.hypot(dx, dy);
        }
        float shown = Math.min(len, r);
        if (len > 0f) { touchX = anchorX + dx / len * shown; touchY = anchorY + dy / len * shown; }
        else { touchX = anchorX; touchY = anchorY; }

        float travel = r / cfg.sensitivity;
        float v = travel > 0f ? Math.min(1f, len / travel) : 0f;
        float dz = cfg.deadzone;
        float out = v <= dz ? 0f : (v - dz) / (1f - dz);
        float nx = len > 0f ? dx / len * out : 0f;
        float ny = len > 0f ? dy / len * out : 0f;
        emit(nx, ny);
    }

    private void release() {
        activeId = -1;
        emit(0f, 0f);
        invalidate();
    }

    private static void emit(float nx, float ny) {
        int sx = Math.round(Math.max(-1f, Math.min(1f, nx)) * 32767f);
        int sy = Math.round(Math.max(-1f, Math.min(1f, ny)) * 32767f);
        TouchOverlay.setAxis(SDL_AXIS_LEFTX, sx);
        TouchOverlay.setAxis(SDL_AXIS_LEFTY, sy);
    }

    /** Called when edit mode starts/stops so a held stick is never left deflected. */
    void resetInput() {
        activeId = -1;
        emit(0f, 0f);
        invalidate();
    }

    // ── edit ────────────────────────────────────────────────────────────
    private boolean handleEdit(MotionEvent ev) {
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                float x = ev.getX(), y = ev.getY();
                if (Math.hypot(x - homeX(), y - homeY()) > radius() * 1.3f) return false;
                dragDx = x - homeX(); dragDy = y - homeY();
                if (editListener != null) editListener.onSelect();
                return true;
            }
            case MotionEvent.ACTION_MOVE:
                item.x = TouchLayoutConfig.clamp((ev.getX() - dragDx) / getWidth(), 0.02f, 0.98f);
                item.y = TouchLayoutConfig.clamp((ev.getY() - dragDy) / getHeight(), 0.04f, 0.96f);
                invalidate();
                if (editListener != null) editListener.onMoved();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                if (editListener != null) editListener.onDrop();
                return true;
            default:
                return true;
        }
    }
}
