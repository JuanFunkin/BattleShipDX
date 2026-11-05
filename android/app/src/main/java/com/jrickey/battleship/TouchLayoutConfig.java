package com.jrickey.battleship;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * User-configurable touch layout (BattleShipDX): position, size and visibility of every on-screen
 * control plus global opacity / size / stick behaviour. Stored in SharedPreferences.
 */
final class TouchLayoutConfig {

    static final class Item {
        final String id;
        final String label;
        float x, y;          // centre, as a fraction of the screen (0..1)
        float scale = 1f;    // per-control size multiplier
        boolean visible = true;
        final float defX, defY;
        Item(String id, String label, float x, float y) {
            this.id = id; this.label = label; this.x = x; this.y = y; this.defX = x; this.defY = y;
        }
    }

    static final String STICK = "stick";

    final Map<String, Item> items = new LinkedHashMap<>();

    float opacity = 0.70f;      // 0.15 .. 1.0
    float sizeScale = 1.0f;     // 0.6 .. 1.6 (all controls)
    float deadzone = 0.06f;     // 0 .. 0.4
    float sensitivity = 1.3f;   // 0.6 .. 2.0 (>1 reaches full deflection with less finger travel)
    boolean floating = true;    // the stick appears where you touch (left side of the screen)
    boolean follow = true;      // the stick base follows your finger when you drag past the ring
    boolean haptics = true;

    private SharedPreferences prefs;

    TouchLayoutConfig() {
        items.put(STICK, new Item(STICK, "Stick", 0.17f, 0.66f));
        items.put("A",     new Item("A",     "A",     0.90f, 0.72f));
        items.put("B",     new Item("B",     "B",     0.78f, 0.82f));
        items.put("Jump",  new Item("Jump",  "Jump",  0.88f, 0.44f));
        items.put("Z",     new Item("Z",     "Z",     0.72f, 0.13f));
        items.put("R",     new Item("R",     "R",     0.88f, 0.13f));
        items.put("Start", new Item("Start", "Start", 0.50f, 0.90f));
    }

    void load(Context ctx) {
        prefs = ctx.getSharedPreferences("ssb64_touch_layout", Context.MODE_PRIVATE);
        // Layout v2: new, more comfortable stick defaults. Reset the stick tuning saved by older builds once.
        if (prefs.getInt("layoutVersion", 1) < 2) {
            prefs.edit().remove("deadzone").remove("sensitivity").remove(STICK + ".s")
                 .putInt("layoutVersion", 2).apply();
        }
        opacity = clamp(prefs.getFloat("opacity", opacity), 0.15f, 1f);
        sizeScale = clamp(prefs.getFloat("sizeScale", sizeScale), 0.6f, 1.6f);
        deadzone = clamp(prefs.getFloat("deadzone", deadzone), 0f, 0.4f);
        sensitivity = clamp(prefs.getFloat("sensitivity", sensitivity), 0.6f, 2.0f);
        floating = prefs.getBoolean("floating", floating);
        follow = prefs.getBoolean("follow", follow);
        haptics = prefs.getBoolean("haptics", haptics);
        for (Item it : items.values()) {
            it.x = clamp(prefs.getFloat(it.id + ".x", it.defX), 0.02f, 0.98f);
            it.y = clamp(prefs.getFloat(it.id + ".y", it.defY), 0.04f, 0.96f);
            it.scale = clamp(prefs.getFloat(it.id + ".s", 1f), 0.5f, 2.2f);
            it.visible = prefs.getBoolean(it.id + ".v", true);
        }
    }

    void save() {
        if (prefs == null) return;
        SharedPreferences.Editor e = prefs.edit();
        e.putFloat("opacity", opacity).putFloat("sizeScale", sizeScale)
         .putFloat("deadzone", deadzone).putFloat("sensitivity", sensitivity)
         .putBoolean("floating", floating).putBoolean("follow", follow).putBoolean("haptics", haptics);
        for (Item it : items.values()) {
            e.putFloat(it.id + ".x", it.x).putFloat(it.id + ".y", it.y)
             .putFloat(it.id + ".s", it.scale).putBoolean(it.id + ".v", it.visible);
        }
        e.apply();
    }

    void resetAll() {
        opacity = 0.70f; sizeScale = 1f; deadzone = 0.06f; sensitivity = 1.3f;
        floating = true; follow = true; haptics = true;
        for (Item it : items.values()) { it.x = it.defX; it.y = it.defY; it.scale = 1f; it.visible = true; }
        save();
    }

    static float clamp(float v, float lo, float hi) { return Math.max(lo, Math.min(hi, v)); }
}
