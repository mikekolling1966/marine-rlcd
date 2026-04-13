#include "Display_RLCD.h"

// Global DisplayPort instance
DisplayPort *g_rlcd = nullptr;

// Dummy panel_handle (kept for LVGL_Driver compatibility)
void *panel_handle = nullptr;

// Backlight level stub
uint8_t LCD_Backlight = 100;

void LCD_Init() {
    g_rlcd = new DisplayPort(RLCD_MOSI, RLCD_SCL, RLCD_DC, RLCD_CS, RLCD_RST,
                              ESP_PANEL_LCD_WIDTH, ESP_PANEL_LCD_HEIGHT);
    g_rlcd->RLCD_Init();
    g_rlcd->RLCD_ColorClear(ColorWhite);
    g_rlcd->RLCD_Display();
}

void Set_Backlight(uint8_t level) {
    LCD_Backlight = level;
    // RLCD has no backlight PWM — reflective display
}
