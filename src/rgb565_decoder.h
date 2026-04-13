#ifndef RGB565_DECODER_H
#define RGB565_DECODER_H

#include "lvgl.h"

// Initialize RGB565 binary decoder for LVGL
void rgb565_decoder_init(void);

// Pre-load a single .bin file into the PSRAM cache.
// Call before WiFi starts so first-access SD reads don't race WiFi DMA.
void rgb565_preload_file(const char* lv_path);

// Clear cached RGB565 image data so updated assets with the same filename reload from SD.
void rgb565_cache_clear(void);

#endif // RGB565_DECODER_H
