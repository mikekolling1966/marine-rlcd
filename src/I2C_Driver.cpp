#include "I2C_Driver.h"                    

void I2C_Init(void) {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
}
// 寄存器地址为 8 位的
esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
  Wire.beginTransmission(Driver_addr);
  Wire.write(Reg_addr); 
  uint8_t tx_status = Wire.endTransmission(false);
  if (tx_status != 0) {
    printf("The I2C transmission fails. - I2C Read addr=0x%02X reg=0x%02X rc=%u\r\n",
           Driver_addr, Reg_addr, tx_status);
    return ESP_FAIL;
  }

  size_t bytes_read = Wire.requestFrom((int)Driver_addr, (int)Length, (int)true);
  if (bytes_read != Length) {
    printf("The I2C read length is short. addr=0x%02X reg=0x%02X want=%lu got=%u\r\n",
           Driver_addr, Reg_addr, (unsigned long)Length, (unsigned)bytes_read);
    return ESP_FAIL;
  }

  for (uint32_t i = 0; i < Length; i++) {
    *Reg_data++ = Wire.read();
  }
  return ESP_OK;
}
esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
  Wire.beginTransmission(Driver_addr);
  Wire.write(Reg_addr);       
  for (uint32_t i = 0; i < Length; i++) {
    Wire.write(*Reg_data++);
  }
  uint8_t tx_status = Wire.endTransmission(true);
  if (tx_status != 0)
  {
    printf("The I2C transmission fails. - I2C Write addr=0x%02X reg=0x%02X rc=%u\r\n",
           Driver_addr, Reg_addr, tx_status);
    return ESP_FAIL;
  }
  return ESP_OK;
}

bool I2C_DevicePresent(uint8_t Driver_addr)
{
  Wire.beginTransmission(Driver_addr);
  uint8_t tx_status = Wire.endTransmission(true);
  return tx_status == 0;
}

void I2C_Scan(void)
{
  bool found_any = false;
  printf("[I2C] Scanning bus...\r\n");
  for (uint8_t addr = 1; addr < 127; ++addr) {
    if (I2C_DevicePresent(addr)) {
      printf("[I2C] Found device at 0x%02X\r\n", addr);
      found_any = true;
    }
    delay(1);
  }

  if (!found_any) {
    printf("[I2C] No devices found on bus\r\n");
  }
}
