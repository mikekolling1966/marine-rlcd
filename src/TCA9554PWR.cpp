// TCA9554PWR — stubbed out: not present on Waveshare RLCD board
#include "TCA9554PWR.h"

uint8_t g_tca9554_address = TCA9554_ADDR_V3;

bool detect_expander_address()    { return false; }
bool is_board_v4()                { return false; }
uint8_t exio_output_reg()         { return TCA9554_OUTPUT_REG; }
uint8_t pin_tp_rst()              { return EXIO_PIN1; }
uint8_t pin_lcd_rst()             { return EXIO_PIN3; }
uint8_t pin_sdcs()                { return EXIO_PIN4; }
uint8_t I2C_Read_EXIO(uint8_t)   { return 0; }
uint8_t I2C_Write_EXIO(uint8_t, uint8_t) { return 0; }
void Mode_EXIO(uint8_t, uint8_t) {}
void Mode_EXIOS(uint8_t)         {}
uint8_t Read_EXIO(uint8_t)       { return 0; }
uint8_t Read_EXIOS(uint8_t)      { return 0; }
void Set_EXIO(uint8_t, uint8_t)  {}
void Set_EXIOS(uint8_t)          {}
void Set_Toggle(uint8_t)         {}
void TCA9554PWR_Init(uint8_t)    {}
void backlight_set_pwm(uint8_t)  {}
