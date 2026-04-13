#pragma once
// Waveshare RLCD (400x300 reflective SPI LCD) driver
// Drop-in replacement for Display_ST7701.h

#include "display_bsp.h"   // DisplayPort class
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Physical screen resolution for the Waveshare ESP32-S3-RLCD-4.2 panel.
// The module's active area is landscape even though some datasheets quote the
// raw matrix as 300x400.
#define ESP_PANEL_LCD_WIDTH   400
#define ESP_PANEL_LCD_HEIGHT  300

// The existing UI was authored as a square 480x480 layout. Keep that logical
// canvas and scale it to the portrait RLCD so the whole interface remains
// visible without distorting round gauges into ovals.
#define UI_LOGICAL_WIDTH      480
#define UI_LOGICAL_HEIGHT     480

// Stubs for ST7701 defines referenced in main.cpp / network_setup.cpp
#define LCD_MOSI_PIN                      12
#define LCD_CLK_PIN                       11
#define ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ  10000000

// Pin mapping: MOSI=12, SCL=11, DC=5, CS=40, RST=41
#define RLCD_MOSI  12
#define RLCD_SCL   11
#define RLCD_DC    5
#define RLCD_CS    40
#define RLCD_RST   41

// Luminance threshold for colour→mono conversion.
// Pixels with luminance ABOVE this become black ink;
// pixels BELOW become white (reflective background).
// Range 0-255.  Lower = more pixels appear as ink.
#define MONO_THRESHOLD 64

// Global DisplayPort instance — used by LVGL flush callback
extern DisplayPort *g_rlcd;

// Dummy panel_handle (LVGL_Driver stores this in disp_drv.user_data)
extern void *panel_handle;

// Backlight level (0-100, ignored on RLCD — no backlight control)
extern uint8_t LCD_Backlight;
#define Backlight_MAX 100

void LCD_Init();
void Set_Backlight(uint8_t level);   // no-op on RLCD
