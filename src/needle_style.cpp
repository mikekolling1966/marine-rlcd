#include "needle_style.h"
#include "network_setup.h"
#include "ui.h"
#include <SD_MMC.h>

// Defaults matching current hard-coded values in main/UI
static const uint16_t DEFAULT_CX = 240;
static const uint16_t DEFAULT_CY = 240;
// Top needle defaults
static const int16_t DEFAULT_TOP_INNER = 142;
static const int16_t DEFAULT_TOP_OUTER = 210;
static const uint16_t DEFAULT_TOP_WIDTH = 10;
// Bottom needle defaults
static const int16_t DEFAULT_BOT_INNER = 142;
static const int16_t DEFAULT_BOT_OUTER = 200;
static const uint16_t DEFAULT_BOT_WIDTH = 8;

static const char *NEEDLE_STYLE_FILE = "/config/needle_styles.bin";

struct NeedleStyleRecord {
    char color[8];
    uint16_t width;
    int16_t inner;
    int16_t outer;
    uint16_t cx;
    uint16_t cy;
    uint8_t rounded;
    uint8_t gradient;
    uint8_t foreground;
    uint8_t reserved;
};

static NeedleStyleRecord g_needle_style_records[NUM_SCREENS][2];
static bool g_needle_style_cache_loaded = false;

void needle_style_init_defaults() {
    // Nothing to do; defaults are applied on-the-fly
}

static NeedleStyle default_needle_style(int gauge) {
    NeedleStyle s;
    s.cx = DEFAULT_CX;
    s.cy = DEFAULT_CY;
    if (gauge == 0) {
        s.inner = DEFAULT_TOP_INNER;
        s.outer = DEFAULT_TOP_OUTER;
        s.width = DEFAULT_TOP_WIDTH;
        s.color = String("#FFFFFF");
    } else {
        s.inner = DEFAULT_BOT_INNER;
        s.outer = DEFAULT_BOT_OUTER;
        s.width = DEFAULT_BOT_WIDTH;
        s.color = String("#FF8800");
    }
    s.rounded = false;
    s.gradient = false;
    s.foreground = true;
    return s;
}

static NeedleStyleRecord style_to_record(const NeedleStyle& s) {
    NeedleStyleRecord rec = {};
    strncpy(rec.color, s.color.c_str(), sizeof(rec.color) - 1);
    rec.color[sizeof(rec.color) - 1] = '\0';
    rec.width = s.width;
    rec.inner = s.inner;
    rec.outer = s.outer;
    rec.cx = s.cx;
    rec.cy = s.cy;
    rec.rounded = s.rounded ? 1 : 0;
    rec.gradient = s.gradient ? 1 : 0;
    rec.foreground = s.foreground ? 1 : 0;
    return rec;
}

static NeedleStyle record_to_style(const NeedleStyleRecord& rec, int gauge) {
    NeedleStyle s = default_needle_style(gauge);
    if (rec.color[0] == '#') {
        s.color = String(rec.color);
    }
    s.width = rec.width;
    s.inner = rec.inner;
    s.outer = rec.outer;
    s.cx = rec.cx;
    s.cy = rec.cy;
    s.rounded = rec.rounded != 0;
    s.gradient = rec.gradient != 0;
    s.foreground = rec.foreground != 0;
    return s;
}

static void init_needle_style_cache_defaults() {
    for (int screen = 0; screen < NUM_SCREENS; ++screen) {
        for (int gauge = 0; gauge < 2; ++gauge) {
            g_needle_style_records[screen][gauge] = style_to_record(default_needle_style(gauge));
        }
    }
}

static bool ensure_needle_style_config_dir() {
    if (SD_MMC.exists("/config")) {
        return true;
    }
    return SD_MMC.mkdir("/config");
}

static void load_needle_style_cache() {
    if (g_needle_style_cache_loaded) {
        return;
    }

    init_needle_style_cache_defaults();

    File f = SD_MMC.open(NEEDLE_STYLE_FILE, FILE_READ);
    if (f) {
        const size_t want = sizeof(g_needle_style_records);
        const size_t got = f.read(reinterpret_cast<uint8_t*>(g_needle_style_records), want);
        if (got == want) {
            Serial.printf("[NEEDLES] Loaded %s -> %u bytes\n", NEEDLE_STYLE_FILE, (unsigned)got);
        } else {
            Serial.printf("[NEEDLES] Ignoring short read from %s (%u/%u bytes)\n",
                          NEEDLE_STYLE_FILE, (unsigned)got, (unsigned)want);
            init_needle_style_cache_defaults();
        }
        f.close();
    }

    g_needle_style_cache_loaded = true;
}

static bool save_needle_style_cache() {
    if (!ensure_needle_style_config_dir()) {
        Serial.println("[NEEDLES] Failed to ensure /config exists on SD");
        return false;
    }

    const char *tmp_path = "/config/needle_styles.bin.tmp";
    File f = SD_MMC.open(tmp_path, FILE_WRITE);
    if (!f) {
        Serial.printf("[NEEDLES] Failed to open %s for write\n", tmp_path);
        return false;
    }

    const size_t want = sizeof(g_needle_style_records);
    const size_t wrote = f.write(reinterpret_cast<const uint8_t*>(g_needle_style_records), want);
    f.flush();
    f.close();

    if (wrote != want) {
        Serial.printf("[NEEDLES] Short write to %s (%u/%u bytes)\n",
                      tmp_path, (unsigned)wrote, (unsigned)want);
        SD_MMC.remove(tmp_path);
        return false;
    }

    SD_MMC.remove(NEEDLE_STYLE_FILE);
    if (!SD_MMC.rename(tmp_path, NEEDLE_STYLE_FILE)) {
        Serial.printf("[NEEDLES] Failed to rename %s -> %s\n", tmp_path, NEEDLE_STYLE_FILE);
        SD_MMC.remove(tmp_path);
        return false;
    }

    Serial.printf("[NEEDLES] Saved %s -> %u bytes\n", NEEDLE_STYLE_FILE, (unsigned)want);
    return true;
}

NeedleStyle get_needle_style(int screen, int gauge) {
    if (screen < 0 || screen >= NUM_SCREENS || gauge < 0 || gauge > 1) {
        return default_needle_style(gauge == 0 ? 0 : 1);
    }
    load_needle_style_cache();
    return record_to_style(g_needle_style_records[screen][gauge], gauge);
}

void apply_needle_style_to_obj(lv_obj_t* obj, int screen, int gauge) {
    if (!obj) return;
    NeedleStyle s = get_needle_style(screen, gauge);
    // Apply color
    lv_color_t c = lv_color_hex((uint32_t)strtol(s.color.substring(1).c_str(), NULL, 16));
    lv_obj_set_style_line_color(obj, c, 0);
    lv_obj_set_style_line_width(obj, s.width, 0);
    lv_obj_set_style_line_rounded(obj, s.rounded, 0);
    if (s.foreground) lv_obj_move_foreground(obj); else lv_obj_move_background(obj);
}

void apply_all_needle_styles() {
    // Top needles: ui_Needle (screen 0), ui_Needle2 (screen1)...
    apply_needle_style_to_obj(ui_Needle, 0, 0);
    apply_needle_style_to_obj(ui_Lower_Needle, 0, 1);
    apply_needle_style_to_obj(ui_Needle2, 1, 0);
    apply_needle_style_to_obj(ui_Lower_Needle2, 1, 1);
    apply_needle_style_to_obj(ui_Needle3, 2, 0);
    apply_needle_style_to_obj(ui_Lower_Needle3, 2, 1);
    apply_needle_style_to_obj(ui_Needle4, 3, 0);
    apply_needle_style_to_obj(ui_Lower_Needle4, 3, 1);
    apply_needle_style_to_obj(ui_Needle5, 4, 0);
    apply_needle_style_to_obj(ui_Lower_Needle5, 4, 1);
}

void save_needle_style_from_args(int screen, int gauge, const String& color, uint16_t width, int16_t inner, int16_t outer, uint16_t cx, uint16_t cy, bool rounded, bool gradient, bool fg) {
    if (screen < 0 || screen >= NUM_SCREENS || gauge < 0 || gauge > 1) {
        return;
    }
    load_needle_style_cache();
    NeedleStyle s;
    s.color = color;
    s.width = width;
    s.inner = inner;
    s.outer = outer;
    s.cx = cx;
    s.cy = cy;
    s.rounded = rounded;
    s.gradient = gradient;
    s.foreground = fg;
    g_needle_style_records[screen][gauge] = style_to_record(s);
    save_needle_style_cache();
}
