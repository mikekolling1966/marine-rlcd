#pragma once
#include <Wire.h> 
#include "esp_err.h"

#define I2C_SCL_PIN       13
#define I2C_SDA_PIN       14


void I2C_Init(void);
esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length);
esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length);
bool I2C_DevicePresent(uint8_t Driver_addr);
void I2C_Scan(void);
