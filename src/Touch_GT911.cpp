// Touch_GT911 — stubbed out: Waveshare RLCD has no touch screen
#include "Touch_GT911.h"

uint8_t gt911_addr = GT911_ADDR_PRIMARY;
uint8_t Touch_interrupts = 0;

uint8_t Touch_Init() { return 0; }
void    Touch_Loop() {}
uint8_t GT911_Touch_Reset() { return 0; }
void    GT911_Read_cfg() {}
uint8_t Touch_Read_Data() { return 0; }
uint8_t Touch_Get_XY(uint16_t *x, uint16_t *y, uint16_t *strength,
                     uint8_t *point_num, uint8_t max_point_num) {
    if (point_num) *point_num = 0;
    return 0;
}
void example_touchpad_read() {}
void IRAM_ATTR Touch_GT911_ISR() {}
