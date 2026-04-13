#pragma once
// Redirected to Waveshare RLCD driver
#include "Display_RLCD.h"

// Stub out ST7701-specific things referenced in other files
inline void ST7701_Init() {}
inline void LCD_addWindow(uint16_t, uint16_t, uint16_t, uint16_t, uint8_t*) {}
inline void Display_RunDiagnostics() {}
inline void Display_RunDiagnosticsLoop(int, uint32_t) {}
inline void Display_RunDiagnosticsAsync(int, uint32_t) {}
inline void Backlight_Init() {}
inline uint32_t get_vsync_count() { return 0; }
inline uint32_t get_vsync_max_gap_us() { return 0; }
inline void reset_vsync_stats() {}
