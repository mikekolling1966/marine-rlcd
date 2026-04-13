/*****************************************************************************
  | File        :   LVGL_Driver.cpp
  | Modified for Waveshare RLCD (400x300 reflective SPI LCD, monochrome)
******************************************************************************/
#include "LVGL_Driver.h"
#include "esp_timer.h"
#include "SD_Card.h"
#include <SD_MMC.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <WiFi.h>
#include "Display_RLCD.h"

// Screen-off state — defined in main.cpp
extern bool     g_screen_is_off;
extern uint32_t g_last_activity_ms;

static const char *TAG_LVGL = "LVGL";

lv_disp_drv_t disp_drv;

// LVGL filesystem driver callbacks for SD card access
typedef struct {
  bool is_posix;
  File *file;
  FILE *fp;
} lv_file_wrapper_t;

static void * fs_open_cb(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode) {
    LV_UNUSED(drv);
    const char * sd_path = path;
    if (path && strlen(path) > 2 && path[1] == ':' && path[2] == '/') {
      sd_path = path + 2;
    }
    File* file = new File(SD_MMC.open(sd_path, FILE_READ));
    if (*file) {
      lv_file_wrapper_t *w = (lv_file_wrapper_t*)malloc(sizeof(lv_file_wrapper_t));
      w->is_posix = false; w->file = file; w->fp = NULL;
      return (void*)w;
    }
    delete file;
    char alt[256];
    snprintf(alt, sizeof(alt), "/sdcard%s", sd_path);
    FILE *fp = fopen(alt, "rb");
    if (!fp) return NULL;
    lv_file_wrapper_t *w = (lv_file_wrapper_t*)malloc(sizeof(lv_file_wrapper_t));
    w->is_posix = true; w->file = NULL; w->fp = fp;
    return (void*)w;
}

static lv_fs_res_t fs_close_cb(lv_fs_drv_t * drv, void * file_p) {
    LV_UNUSED(drv);
    lv_file_wrapper_t *w = (lv_file_wrapper_t*)file_p;
    if (!w) return LV_FS_RES_OK;
    if (w->is_posix) { fclose(w->fp); } else { w->file->close(); delete w->file; }
    free(w);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read_cb(lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br) {
    LV_UNUSED(drv);
    lv_file_wrapper_t *w = (lv_file_wrapper_t*)file_p;
    if (!w) return LV_FS_RES_UNKNOWN;
    if (w->is_posix) { *br = (uint32_t)fread(buf, 1, btr, w->fp); }
    else { *br = w->file->read((uint8_t *)buf, btr); }
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek_cb(lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence) {
    LV_UNUSED(drv);
    lv_file_wrapper_t *w = (lv_file_wrapper_t*)file_p;
    if (!w) return LV_FS_RES_UNKNOWN;
    if (w->is_posix) {
      int origin = (whence == LV_FS_SEEK_CUR) ? SEEK_CUR : (whence == LV_FS_SEEK_END) ? SEEK_END : SEEK_SET;
      return (fseek(w->fp, (long)pos, origin) == 0) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
    } else {
      SeekMode mode = (whence == LV_FS_SEEK_CUR) ? SeekCur : (whence == LV_FS_SEEK_END) ? SeekEnd : SeekSet;
      w->file->seek(pos, mode);
      return LV_FS_RES_OK;
    }
}

static lv_fs_res_t fs_tell_cb(lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p) {
    LV_UNUSED(drv);
    lv_file_wrapper_t *w = (lv_file_wrapper_t*)file_p;
    if (!w) return LV_FS_RES_UNKNOWN;
    if (w->is_posix) { long p = ftell(w->fp); if (p < 0) return LV_FS_RES_UNKNOWN; *pos_p = (uint32_t)p; }
    else { *pos_p = w->file->position(); }
    return LV_FS_RES_OK;
}

static lv_disp_draw_buf_t draw_buf;
void* buf1 = NULL;
void* buf2 = NULL;
static volatile uint32_t g_flush_max_us = 0;
static volatile uint32_t g_flush_count = 0;
static uint8_t *g_ui_mono_buffer = NULL;

static inline void ui_mono_set(int32_t x, int32_t y, bool white)
{
    if (!g_ui_mono_buffer || x < 0 || y < 0 || x >= LVGL_WIDTH || y >= LVGL_HEIGHT) return;
    uint32_t idx = (uint32_t)y * LVGL_WIDTH + (uint32_t)x;
    uint32_t byte_index = idx >> 3;
    uint8_t bit_mask = (uint8_t)(1U << (7 - (idx & 0x07)));
    if (white) g_ui_mono_buffer[byte_index] |= bit_mask;
    else g_ui_mono_buffer[byte_index] &= (uint8_t)~bit_mask;
}

static inline bool ui_mono_get(int32_t x, int32_t y)
{
    if (!g_ui_mono_buffer || x < 0 || y < 0 || x >= LVGL_WIDTH || y >= LVGL_HEIGHT) return true;
    uint32_t idx = (uint32_t)y * LVGL_WIDTH + (uint32_t)x;
    uint32_t byte_index = idx >> 3;
    uint8_t bit_mask = (uint8_t)(1U << (7 - (idx & 0x07)));
    return (g_ui_mono_buffer[byte_index] & bit_mask) != 0;
}

// No-op remap stubs (kept for API compatibility)
void Lvgl_ToggleRemap() {}
void Lvgl_EnableRemap(bool) {}
void Lvgl_SetRemapTable(const uint8_t[16]) {}
void Lvgl_PrintRemap() {}

void Lvgl_print(const char *) {}

/*
  LVGL flush callback — converts 16-bit RGB565 tiles to monochrome
  and writes them into the RLCD frame buffer.
  When this is the last area of the frame, pushes the full buffer to the display.
*/
void Lvgl_Display_LCD(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t t0 = (uint32_t)esp_timer_get_time();

    if (g_rlcd && g_ui_mono_buffer) {
        for (int32_t y = area->y1; y <= area->y2; y++) {
            for (int32_t x = area->x1; x <= area->x2; x++) {
                // Extract RGB565 components
                uint16_t c = color_p->full;
                uint8_t r = ((c >> 11) & 0x1F) << 3;   // 5-bit → 8-bit
                uint8_t g = ((c >>  5) & 0x3F) << 2;   // 6-bit → 8-bit
                uint8_t b = ( c        & 0x1F) << 3;   // 5-bit → 8-bit

                // BT.601 luminance
                uint16_t lum = ((uint16_t)r * 77 + (uint16_t)g * 150 + (uint16_t)b * 29) >> 8;

                // Invert: bright pixels → black ink, dark pixels → white (background)
                // MONO_THRESHOLD defined in Display_RLCD.h (default 64)
                bool white = !(lum > MONO_THRESHOLD);
                ui_mono_set(x, y, white);
                color_p++;
            }
        }

        // Push full frame when LVGL signals this is the last dirty region
        if (lv_disp_flush_is_last(disp_drv)) {
            const int32_t target_w = (ESP_PANEL_LCD_WIDTH < ESP_PANEL_LCD_HEIGHT)
                                         ? ESP_PANEL_LCD_WIDTH
                                         : ESP_PANEL_LCD_HEIGHT;
            const int32_t target_h = target_w;
            const int32_t offset_x = (ESP_PANEL_LCD_WIDTH - target_w) / 2;
            const int32_t offset_y = (ESP_PANEL_LCD_HEIGHT - target_h) / 2;

            g_rlcd->RLCD_ColorClear(ColorWhite);
            for (int32_t py = 0; py < ESP_PANEL_LCD_HEIGHT; ++py) {
                for (int32_t px = 0; px < ESP_PANEL_LCD_WIDTH; ++px) {
                    uint8_t pixel = ColorWhite;
                    if (px >= offset_x && px < offset_x + target_w &&
                        py >= offset_y && py < offset_y + target_h) {
                        int32_t sx = ((px - offset_x) * LVGL_WIDTH) / target_w;
                        int32_t sy = ((py - offset_y) * LVGL_HEIGHT) / target_h;
                        pixel = ui_mono_get(sx, sy) ? ColorWhite : ColorBlack;
                    }
                    g_rlcd->RLCD_SetLandscapePixel((uint16_t)px, (uint16_t)py, pixel);
                }
            }
            g_rlcd->RLCD_Display();
        }
    }

    uint32_t dur = (uint32_t)esp_timer_get_time() - t0;
    if (dur > g_flush_max_us) g_flush_max_us = dur;
    g_flush_count++;
    lv_disp_flush_ready(disp_drv);
}

// Touchpad read — always returns RELEASED (no touch screen on RLCD)
void Lvgl_Touchpad_Read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    data->state = LV_INDEV_STATE_REL;
}

void example_increase_lvgl_tick(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

void Lvgl_Init(void)
{
    lv_init();

    lv_img_cache_set_size(4);

    // SD filesystem driver
    static lv_fs_drv_t fs_drv;
    lv_fs_drv_init(&fs_drv);
    fs_drv.letter   = 'S';
    fs_drv.open_cb  = fs_open_cb;
    fs_drv.close_cb = fs_close_cb;
    fs_drv.read_cb  = fs_read_cb;
    fs_drv.seek_cb  = fs_seek_cb;
    fs_drv.tell_cb  = fs_tell_cb;
    lv_fs_drv_register(&fs_drv);

    size_t ui_mono_bytes = (LVGL_WIDTH * LVGL_HEIGHT) / 8;
    g_ui_mono_buffer = (uint8_t *)heap_caps_malloc(ui_mono_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_ui_mono_buffer) {
        g_ui_mono_buffer = (uint8_t *)heap_caps_malloc(ui_mono_bytes, MALLOC_CAP_8BIT);
    }
    if (g_ui_mono_buffer) {
        memset(g_ui_mono_buffer, 0xFF, ui_mono_bytes);
    } else {
        ESP_LOGW(TAG_LVGL, "Logical monochrome frame buffer alloc failed");
    }

    // LVGL draw buffers for the original 480x480 square UI.
    // Use 1/8 screen tiles to stay within internal RAM limits.
    size_t lv_buf_px    = (LVGL_WIDTH * LVGL_HEIGHT / 8);
    size_t lv_buf_bytes = lv_buf_px * sizeof(lv_color_t);
    buf1 = heap_caps_malloc(lv_buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    buf2 = heap_caps_malloc(lv_buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        ESP_LOGW(TAG_LVGL, "Full dual-buffer alloc failed, trying single buffer");
        if (!buf1) buf1 = heap_caps_malloc(lv_buf_bytes, MALLOC_CAP_8BIT);
        buf2 = NULL;  // single buffer fallback
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, lv_buf_px);
    ESP_LOGI(TAG_LVGL, "LVGL buffers: buf1=%p buf2=%p px=%u bytes=%u",
             buf1, buf2, (unsigned)lv_buf_px, (unsigned)lv_buf_bytes);

    // Display driver
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res    = LVGL_WIDTH;
    disp_drv.ver_res    = LVGL_HEIGHT;
    disp_drv.flush_cb   = Lvgl_Display_LCD;
    disp_drv.draw_buf   = &draw_buf;
    disp_drv.user_data  = panel_handle;  // NULL for RLCD
    disp_drv.sw_rotate  = 0;
    disp_drv.rotated    = LV_DISP_ROT_NONE;
    disp_drv.direct_mode = 0;
    disp_drv.full_refresh = 0;

    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    lv_disp_set_default(disp);

    // Input device — pointer type with no-op read (no touch)
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = Lvgl_Touchpad_Read;
    lv_indev_drv_register(&indev_drv);

    // Boot label
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Marine RLCD");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
    esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);
}

uint32_t get_flush_max_us()  { return g_flush_max_us; }
uint32_t get_flush_count()   { return g_flush_count; }
void reset_flush_stats()     { g_flush_max_us = 0; g_flush_count = 0; }

void Lvgl_Loop(void)
{
    lv_timer_handler();
}
