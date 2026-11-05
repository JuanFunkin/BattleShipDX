package com.jrickey.battleship;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;

/**
 * On-screen touch controls for the SSB64 port.
 *
 * Layout (landscape):
 *   - Left half: fixed-anchor virtual analog stick ({@link AnalogStickView}).
 *   - Right half: face-button cluster.
 *   - Top corners: shoulder/trigger buttons (Z, R) and the menu hamburger.
 *
 *        [≡]                                          [Z]    [R]
 *
 *
 *                                                        [B]    [A]
 *                                                            [Start]
 *
 * Mappings (match libultraship's N64-to-SDL defaults in
 * ControllerDefaultMappings.cpp):
 *
 *     A     = SDL_CONTROLLER_BUTTON_A
 *     B     = SDL_CONTROLLER_BUTTON_B
 *     Start = SDL_CONTROLLER_BUTTON_START
 *     Z     = SDL_CONTROLLER_AXIS_TRIGGERLEFT  (full positive, axis-mapped — Z is a digital button on real N64, but LUS maps it from a controller trigger axis)
 *     R     = SDL_CONTROLLER_AXIS_TRIGGERRIGHT (full positive — same story)
 *     Menu  = SDL_KEYDOWN F1 (matches Gui.cpp's TOGGLE_BTN)
 *
 * LUS's SDLButtonToAnyMapping picks the virtual gamepad up via the
 * Xbox-360 vendor/product signature attached on the native side
 * (port/android_touch_overlay.cpp); user-configured remappings via the
 * LUS controller menu still apply.
 */
public final class TouchOverlay {

    /** Native methods implemented in port/android_touch_overlay.cpp. */
    public static native void setButton(int sdlButton, boolean down);
    public static native void setAxis(int sdlAxis, int value);
    /** Toggle the LUS menu (deferred to SDL_main, see port/gameloop.cpp). */
    public static native void toggleMenu();
    /** Is the LUS menu currently visible? Polled to hide the overlay so
     *  touches pass through to ImGui. */
    public static native boolean isMenuVisible();

    /** Trigger-axis convention shared by C++ + Java.
     *
     *  SDL_GameController normalizes our virtual joystick's trigger axes
     *  (a4 = lefttrigger, a5 = righttrigger) from the raw range
     *  -32768..+32767 to the GameController range 0..32767. With that
     *  normalization, raw -32768 = released, raw +32767 = fully pressed.
     *  Sending raw 0 lands the trigger at ~50% pressed — which LUS reads
     *  as N64 Z/R held the entire time. */
    public static final int TRIGGER_RELEASED = -32768;
    public static final int TRIGGER_PRESSED  =  32767;

    /** Convenience helper: pressed-or-released semantics for trigger axes. */
    public static void setTrigger(int sdlAxis, boolean pressed) {
        setAxis(sdlAxis, pressed ? TRIGGER_PRESSED : TRIGGER_RELEASED);
    }

    /** SDL_GameControllerButton mirrors. */
    public static final int SDL_CONTROLLER_BUTTON_A             = 0;
    public static final int SDL_CONTROLLER_BUTTON_B             = 1;
    public static final int SDL_CONTROLLER_BUTTON_START         = 6;

    /** SDL_GameControllerAxis mirrors. Triggers go 0..32767. */
    public static final int SDL_CONTROLLER_AXIS_TRIGGERLEFT     = 4;
    public static final int SDL_CONTROLLER_AXIS_TRIGGERRIGHT    = 5;

    private TouchOverlay() { /* static */ }

    /* Lifecycle handles for uninstall(). The InputManager service and the
     * main-looper Handler both hold strong references to what we register
     * with them — without explicit unregistration the whole overlay View
     * hierarchy leaks on every Activity recreation, and the menu poller
     * keeps firing forever. */
    private static InputManager sInputManager;
    private static InputManager.InputDeviceListener sDeviceListener;
    private static Handler sUiHandler;
    private static Runnable sMenuPoller;

    /** Tear down listeners/pollers registered by install(). Call from the
     *  Activity's onDestroy. */
    public static void uninstall() {
        if (sInputManager != null && sDeviceListener != null) {
            sInputManager.unregisterInputDeviceListener(sDeviceListener);
        }
        sInputManager = null;
        sDeviceListener = null;
        if (sUiHandler != null && sMenuPoller != null) {
            sUiHandler.removeCallbacks(sMenuPoller);
        }
        sUiHandler = null;
        sMenuPoller = null;
    }

    // BattleShipDX: user-configurable layout (position / size / opacity), see TouchLayoutConfig.
    private static final int SDL_CONTROLLER_AXIS_RIGHTX = 2;
    private static final int SDL_CONTROLLER_AXIS_RIGHTY = 3;

    private static TouchLayoutConfig sCfg;
    private static OverlayRoot sGameplay;
    private static AnalogStickView sStick;
    private static final java.util.Map<String, ControlButtonView> sButtons = new java.util.LinkedHashMap<>();
    private static ControlButtonView sGear;
    private static View sEditPanel;
    private static boolean sEditing = false;
    private static String sSelected = null;
    private static EditPanelUi sPanelUi;

    /** Positions every control from the saved layout (centre fractions of the screen). */
    static final class OverlayRoot extends FrameLayout {
        OverlayRoot(Context c) { super(c); setClipChildren(false); }
        @Override
        protected void onLayout(boolean changed, int l, int t, int r, int b) {
            int w = r - l, h = b - t;
            for (int i = 0; i < getChildCount(); i++) {
                View v = getChildAt(i);
                if (v instanceof ControlButtonView && ((ControlButtonView) v).item != null) {
                    ControlButtonView cb = (ControlButtonView) v;
                    int d = cb.diameterPx();
                    int cx = Math.round(cb.item.x * w), cy = Math.round(cb.item.y * h);
                    v.measure(MeasureSpec.makeMeasureSpec(d, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(d, MeasureSpec.EXACTLY));
                    v.layout(cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d);
                } else {
                    v.measure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY));
                    v.layout(0, 0, w, h);
                }
            }
        }
    }

    /**
     * Build the overlay and addContentView() it onto the Activity. Must
     * be called AFTER super.onCreate so SDLActivity has already installed
     * its surface as the root content view.
     */
    public static void install(Activity activity) {
        FrameLayout root = new FrameLayout(activity);
        sCfg = new TouchLayoutConfig();
        sCfg.load(activity);
        sButtons.clear();

        // gameplayLayer: stick + game buttons (hidden when a gamepad is paired or the LUS menu is open).
        // menuLayer: the hamburger and the settings (gear) button, same visibility rules.
        OverlayRoot gameplayLayer = new OverlayRoot(activity);
        FrameLayout menuLayer = new FrameLayout(activity);
        sGameplay = gameplayLayer;

        sStick = new AnalogStickView(activity, sCfg);
        gameplayLayer.addView(sStick);

        addGameButton(activity, gameplayLayer, "A",     "A",     0xFF3B82F6, 84, down -> setButton(SDL_CONTROLLER_BUTTON_A, down));
        addGameButton(activity, gameplayLayer, "B",     "B",     0xFF22C55E, 72, down -> setButton(SDL_CONTROLLER_BUTTON_B, down));
        addGameButton(activity, gameplayLayer, "Jump",  "Jump",  0xFFEAB308, 62, down -> setAxis(SDL_CONTROLLER_AXIS_RIGHTY, down ? -32767 : 0)); // C-up = jump
        addGameButton(activity, gameplayLayer, "Z",     "Z",     0xFF9CA3AF, 62, down -> setTrigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT, down));
        addGameButton(activity, gameplayLayer, "R",     "R",     0xFF9CA3AF, 62, down -> setTrigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, down));
        addGameButton(activity, gameplayLayer, "Start", "Start", 0xFFEF4444, 54, down -> setButton(SDL_CONTROLLER_BUTTON_START, down));

        // Menu hamburger (top-left) and settings gear next to it. Fixed size, not editable.
        ControlButtonView menuBtn = new ControlButtonView(activity, sCfg, null, "\u2261", 0xFF6B7280, 46);
        FrameLayout.LayoutParams mlp = new FrameLayout.LayoutParams(dp(activity, 46), dp(activity, 46), Gravity.TOP | Gravity.START);
        mlp.leftMargin = dp(activity, 16); mlp.topMargin = dp(activity, 12);
        menuBtn.pressListener = down -> { if (!down && !sEditing) toggleMenu(); };
        menuLayer.addView(menuBtn, mlp);
        sGear = new ControlButtonView(activity, sCfg, null, "\u2699", 0xFF6B7280, 46);
        FrameLayout.LayoutParams glp = new FrameLayout.LayoutParams(dp(activity, 46), dp(activity, 46), Gravity.TOP | Gravity.START);
        glp.leftMargin = dp(activity, 16 + 46 + 10); glp.topMargin = dp(activity, 12);
        sGear.pressListener = down -> { if (!down) setEditMode(activity, true); };
        menuLayer.addView(sGear, glp);

        root.addView(gameplayLayer, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        root.addView(menuLayer, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        sPanelUi = new EditPanelUi(activity);
        sEditPanel = sPanelUi.build();
        sEditPanel.setVisibility(View.GONE);
        FrameLayout.LayoutParams plp = new FrameLayout.LayoutParams(dp(activity, 330), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        root.addView(sEditPanel, plp);

        activity.addContentView(root,
            new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        applyEditState();
        installControllerWatcher(activity, gameplayLayer, menuLayer);
        installMenuVisibilityPoller(activity, gameplayLayer, menuLayer);
    }

    private interface Press { void on(boolean down); }

    private static void addGameButton(Context ctx, OverlayRoot layer, String id, String label, int color, int baseDp, Press p) {
        ControlButtonView v = new ControlButtonView(ctx, sCfg, sCfg.items.get(id), label, color, baseDp);
        v.pressListener = p::on;
        v.editListener = new ControlButtonView.EditListener() {
            @Override public void onSelect(ControlButtonView b) { selectControl(b.item.id); }
            @Override public void onMoved(ControlButtonView b) { }
            @Override public void onDrop(ControlButtonView b) { sCfg.save(); }
        };
        layer.addView(v);
        sButtons.put(id, v);
    }

    // ── edit mode ───────────────────────────────────────────────────────
    private static void releaseAllInputs() {
        setButton(SDL_CONTROLLER_BUTTON_A, false);
        setButton(SDL_CONTROLLER_BUTTON_B, false);
        setButton(SDL_CONTROLLER_BUTTON_START, false);
        setTrigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT, false);
        setTrigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, false);
        setAxis(SDL_CONTROLLER_AXIS_RIGHTX, 0);
        setAxis(SDL_CONTROLLER_AXIS_RIGHTY, 0);
        if (sStick != null) sStick.resetInput();
    }

    private static void setEditMode(Context ctx, boolean on) {
        try { releaseAllInputs(); } catch (UnsatisfiedLinkError ignored) { }
        sEditing = on;
        if (!on) { sSelected = null; sCfg.save(); }
        applyEditState();
    }

    private static void applyEditState() {
        if (sCfg == null) return;
        for (ControlButtonView v : sButtons.values()) {
            v.editMode = sEditing;
            v.selected = sEditing && v.item.id.equals(sSelected);
            v.setVisibility(sEditing || v.item.visible ? View.VISIBLE : View.GONE);
            v.invalidate();
        }
        if (sStick != null) {
            sStick.editMode = sEditing;
            sStick.selected = sEditing && TouchLayoutConfig.STICK.equals(sSelected);
            sStick.editListener = new AnalogStickView.EditListener() {
                @Override public void onSelect() { selectControl(TouchLayoutConfig.STICK); }
                @Override public void onMoved() { }
                @Override public void onDrop() { sCfg.save(); }
            };
            sStick.invalidate();
        }
        if (sEditPanel != null) sEditPanel.setVisibility(sEditing ? View.VISIBLE : View.GONE);
        if (sGear != null) sGear.setVisibility(sEditing ? View.INVISIBLE : View.VISIBLE);
        if (sPanelUi != null) sPanelUi.refresh();
        if (sGameplay != null) sGameplay.requestLayout();
    }

    private static void selectControl(String id) {
        sSelected = id;
        applyEditState();
    }

    /** Re-layout and redraw after a setting changed. */
    private static void relayoutAll() {
        if (sGameplay != null) sGameplay.requestLayout();
        for (ControlButtonView v : sButtons.values()) v.invalidate();
        if (sStick != null) sStick.invalidate();
    }

    /** The edit panel: sliders and switches built from plain Android views. */
    private static final class EditPanelUi {
        private final Context ctx;
        private TextView selTitle;
        private android.widget.SeekBar selSize;
        private android.widget.CheckBox selVisible;
        private android.widget.LinearLayout body;
        private boolean updating = false;

        EditPanelUi(Context c) { ctx = c; }

        private interface FloatCb { void set(float v); }

        private android.widget.SeekBar slider(android.widget.LinearLayout parent, String title, float lo, float hi, float cur, String unit, FloatCb cb, TextView[] outLabel) {
            TextView label = new TextView(ctx);
            label.setTextColor(Color.WHITE);
            label.setTextSize(13f);
            label.setPadding(0, dp(ctx, 8), 0, 0);
            parent.addView(label);
            android.widget.SeekBar sb = new android.widget.SeekBar(ctx);
            sb.setMax(1000);
            sb.setProgress(Math.round((cur - lo) / (hi - lo) * 1000f));
            label.setText(title + ": " + Math.round(cur * 100f) + unit);
            sb.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
                @Override public void onProgressChanged(android.widget.SeekBar s, int p, boolean user) {
                    float v = lo + (hi - lo) * p / 1000f;
                    label.setText(title + ": " + Math.round(v * 100f) + unit);
                    if (user && !updating) { cb.set(v); relayoutAll(); }
                }
                @Override public void onStartTrackingTouch(android.widget.SeekBar s) { }
                @Override public void onStopTrackingTouch(android.widget.SeekBar s) { sCfg.save(); }
            });
            parent.addView(sb);
            if (outLabel != null) outLabel[0] = label;
            return sb;
        }

        private android.widget.CheckBox check(android.widget.LinearLayout parent, String text, boolean cur, android.widget.CompoundButton.OnCheckedChangeListener l) {
            android.widget.CheckBox cb = new android.widget.CheckBox(ctx);
            cb.setText(text);
            cb.setTextColor(Color.WHITE);
            cb.setTextSize(13f);
            cb.setChecked(cur);
            cb.setOnCheckedChangeListener(l);
            parent.addView(cb);
            return cb;
        }

        View build() {
            android.widget.LinearLayout panel = new android.widget.LinearLayout(ctx);
            panel.setOrientation(android.widget.LinearLayout.VERTICAL);
            android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
            bg.setColor(Color.argb(0xE8, 0x12, 0x14, 0x1C));
            bg.setCornerRadius(dp(ctx, 14));
            bg.setStroke(dp(ctx, 1), Color.argb(0x80, 255, 255, 255));
            panel.setBackground(bg);
            panel.setPadding(dp(ctx, 14), dp(ctx, 10), dp(ctx, 14), dp(ctx, 10));

            TextView title = new TextView(ctx);
            title.setText("Edit controls");
            title.setTextColor(Color.WHITE);
            title.setTextSize(17f);
            title.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            panel.addView(title);
            TextView hint = new TextView(ctx);
            hint.setText("Drag a control to move it. Tap it to change its size or hide it.");
            hint.setTextColor(Color.argb(0xCC, 255, 255, 255));
            hint.setTextSize(12f);
            panel.addView(hint);

            android.widget.ScrollView sv = new android.widget.ScrollView(ctx);
            body = new android.widget.LinearLayout(ctx);
            body.setOrientation(android.widget.LinearLayout.VERTICAL);
            sv.addView(body);
            int maxH = Math.round(ctx.getResources().getDisplayMetrics().heightPixels * 0.55f);
            panel.addView(sv, new android.widget.LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, maxH));

            // Selected control
            selTitle = new TextView(ctx);
            selTitle.setTextColor(Color.rgb(255, 214, 10));
            selTitle.setTextSize(14f);
            selTitle.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            selTitle.setPadding(0, dp(ctx, 8), 0, 0);
            body.addView(selTitle);
            selSize = slider(body, "Size of this control", 0.5f, 2.2f, 1f, "%", v -> {
                TouchLayoutConfig.Item it = sCfg.items.get(sSelected);
                if (it != null) it.scale = v;
            }, null);
            selVisible = check(body, "Show this control", true, (b, on) -> {
                if (updating) return;
                TouchLayoutConfig.Item it = sCfg.items.get(sSelected);
                if (it != null) { it.visible = on; sCfg.save(); applyEditState(); }
            });

            // Global
            TextView g = new TextView(ctx);
            g.setText("All controls");
            g.setTextColor(Color.rgb(120, 180, 255));
            g.setTextSize(14f);
            g.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            g.setPadding(0, dp(ctx, 12), 0, 0);
            body.addView(g);
            slider(body, "Opacity", 0.15f, 1f, sCfg.opacity, "%", v -> sCfg.opacity = v, null);
            slider(body, "Size", 0.6f, 1.6f, sCfg.sizeScale, "%", v -> sCfg.sizeScale = v, null);
            check(body, "Vibrate on touch", sCfg.haptics, (b, on) -> { sCfg.haptics = on; sCfg.save(); });

            TextView s = new TextView(ctx);
            s.setText("Stick");
            s.setTextColor(Color.rgb(120, 180, 255));
            s.setTextSize(14f);
            s.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            s.setPadding(0, dp(ctx, 12), 0, 0);
            body.addView(s);
            check(body, "Floating stick (appears where you touch)", sCfg.floating, (b, on) -> { sCfg.floating = on; sCfg.save(); relayoutAll(); });
            check(body, "Base follows your finger", sCfg.follow, (b, on) -> { sCfg.follow = on; sCfg.save(); });
            slider(body, "Dead zone", 0f, 0.4f, sCfg.deadzone, "%", v -> sCfg.deadzone = v, null);
            slider(body, "Sensitivity", 0.6f, 2.0f, sCfg.sensitivity, "%", v -> sCfg.sensitivity = v, null);

            android.widget.LinearLayout row = new android.widget.LinearLayout(ctx);
            row.setOrientation(android.widget.LinearLayout.HORIZONTAL);
            row.setPadding(0, dp(ctx, 10), 0, 0);
            android.widget.Button reset = new android.widget.Button(ctx);
            reset.setText("Reset layout");
            reset.setOnClickListener(v -> { sCfg.resetAll(); sSelected = null; setEditMode(ctx, false); sPanelUi.rebuild(); setEditMode(ctx, true); });
            android.widget.Button done = new android.widget.Button(ctx);
            done.setText("Done");
            done.setOnClickListener(v -> setEditMode(ctx, false));
            row.addView(reset, new android.widget.LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            row.addView(done, new android.widget.LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            panel.addView(row);
            panelRoot = panel;
            return panel;
        }

        private android.widget.LinearLayout panelRoot;

        /** Rebuild the controls so the sliders show the reset values. */
        void rebuild() {
            if (panelRoot == null) return;
            ViewGroup parent = (ViewGroup) panelRoot.getParent();
            if (parent == null) return;
            int idx = parent.indexOfChild(panelRoot);
            ViewGroup.LayoutParams lp = panelRoot.getLayoutParams();
            parent.removeView(panelRoot);
            View fresh = build();
            fresh.setVisibility(View.GONE);
            parent.addView(fresh, idx, lp);
            sEditPanel = fresh;
        }

        void refresh() {
            if (selTitle == null) return;
            updating = true;
            TouchLayoutConfig.Item it = sSelected == null ? null : sCfg.items.get(sSelected);
            if (it == null) {
                selTitle.setText("Selected: (tap a control)");
                selSize.setEnabled(false);
                selVisible.setEnabled(false);
            } else {
                selTitle.setText("Selected: " + it.label);
                selSize.setEnabled(true);
                selVisible.setEnabled(true);
                selSize.setProgress(Math.round((it.scale - 0.5f) / (2.2f - 0.5f) * 1000f));
                selVisible.setChecked(it.visible);
            }
            updating = false;
        }
    }

    /**
     * Single source of truth for overlay visibility. Both layers are hidden
     * when either condition holds:
     *
     *   - a physical gamepad is in control — its buttons replace the on-screen
     *     stick/face cluster, and its Select button opens the LUS menu, so the
     *     hamburger is redundant too; or
     *   - the LUS menu is open — touches must fall through to ImGui, otherwise
     *     the stick area swallows taps on menu items.
     *
     * Idempotent: {@link View#setVisibility} no-ops when the value is
     * unchanged, so this is safe to call every poll tick and on every
     * input-device change.
     */
    private static void refreshOverlayVisibility(View gameplayLayer, View menuLayer) {
        boolean menuOpen;
        try {
            menuOpen = isMenuVisible();
        } catch (UnsatisfiedLinkError e) {
            // Native side not yet ready (pre-SDL_main) — treat menu as closed.
            menuOpen = false;
        }
        int vis = (menuOpen || countGamepads() > 0) ? View.GONE : View.VISIBLE;
        gameplayLayer.setVisibility(vis);
        menuLayer.setVisibility(vis);
    }

    /**
     * Poll overlay visibility on a 100ms cadence. We poll because the menu
     * can close from inside (X button, ESC), not just from our hamburger tap,
     * and neither event pushes a signal to Java. 100ms is cheap and
     * imperceptible. The actual show/hide decision lives in
     * {@link #refreshOverlayVisibility}.
     */
    private static void installMenuVisibilityPoller(Activity activity,
                                                    View gameplayLayer,
                                                    View menuLayer) {
        final Handler ui = new Handler(Looper.getMainLooper());
        Runnable poller = new Runnable() {
            @Override public void run() {
                refreshOverlayVisibility(gameplayLayer, menuLayer);
                ui.postDelayed(this, 100);
            }
        };
        sUiHandler = ui;
        sMenuPoller = poller;
        ui.postDelayed(poller, 100);
    }

    /**
     * Auto-hide the whole overlay (stick + face buttons AND the menu
     * hamburger) when a physical controller is paired. SDL2's
     * SDLControllerManager reports paired controllers to the native side
     * independently of our virtual joystick, so without this LUS sees two
     * controllers (real + virtual) and the user gets duplicate input.
     *
     * The menu hamburger hides along with the gameplay controls: a paired
     * pad's Select button opens the LUS menu (matching libultraship's
     * default), so the on-screen shortcut is redundant. Both layers share
     * one visibility rule — see {@link #refreshOverlayVisibility}.
     *
     * Detection is via Android's InputManager — we count every device
     * whose source mask includes SOURCE_GAMEPAD or SOURCE_JOYSTICK and
     * isn't the per-app "Virtual" device. logcat tag `ssb64.touch`
     * dumps every enumeration so you can debug missing-pad cases.
     */
    private static final String TAG = "ssb64.touch";

    private static void installControllerWatcher(Activity activity,
                                                 View gameplayLayer,
                                                 View menuLayer) {
        Object svc = activity.getSystemService(Context.INPUT_SERVICE);
        if (!(svc instanceof InputManager)) {
            Log.w(TAG, "InputManager unavailable, overlay always visible");
            return;
        }
        final InputManager im = (InputManager) svc;
        final Handler ui = new Handler(Looper.getMainLooper());

        Runnable refresh = () -> {
            int count = countGamepads();
            Log.i(TAG, "controller-watch: gamepads=" + count
                     + " overlay=" + (count > 0 ? "hidden" : "shown"));
            refreshOverlayVisibility(gameplayLayer, menuLayer);
        };

        ui.post(refresh);

        InputManager.InputDeviceListener listener = new InputManager.InputDeviceListener() {
            @Override public void onInputDeviceAdded(int id)   { Log.i(TAG, "input device added id=" + id);   ui.post(refresh); }
            @Override public void onInputDeviceRemoved(int id) { Log.i(TAG, "input device removed id=" + id); ui.post(refresh); }
            @Override public void onInputDeviceChanged(int id) { Log.i(TAG, "input device changed id=" + id); ui.post(refresh); }
        };
        sInputManager = im;
        sDeviceListener = listener;
        im.registerInputDeviceListener(listener, ui);
    }

    private static int countGamepads() {
        int count = 0;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice dev = InputDevice.getDevice(id);
            if (dev == null) continue;
            int sources = dev.getSources();
            boolean isGamepad =
                (sources & InputDevice.SOURCE_GAMEPAD)  == InputDevice.SOURCE_GAMEPAD
             || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
            // Android's internal "Virtual" device on every app process
            // advertises SOURCE_KEYBOARD only, but some OEM ROMs flag it
            // as SOURCE_GAMEPAD too. Filter by name so a pair-less device
            // doesn't auto-hide the overlay.
            String name = dev.getName();
            boolean isBuiltinVirtual = (name != null) &&
                (name.equalsIgnoreCase("Virtual") || name.startsWith("uinput"));
            Log.d(TAG, "  device id=" + id + " name='" + name + "' sources=0x"
                + Integer.toHexString(sources) + " gamepad=" + isGamepad
                + " builtin=" + isBuiltinVirtual);
            if (isGamepad && !isBuiltinVirtual) count++;
        }
        return count;
    }

    private static int dp(Context ctx, int dp) {
        return Math.round(dp * ctx.getResources().getDisplayMetrics().density);
    }
}
