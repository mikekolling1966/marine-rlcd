// Gyro_QMI8658 — stubbed out: not present on Waveshare RLCD board
#include "Gyro_QMI8658.h"

IMUdata Accel = {0, 0, 0};
IMUdata Gyro  = {0, 0, 0};

void QMI8658_Init() {}
void QMI8658_Loop() {}
void QMI8658_transmit(uint8_t, uint8_t) {}
uint8_t QMI8658_receive(uint8_t) { return 0; }
void QMI8658_CTRL9_Write(uint8_t) {}
void QMI8658_sensor_update() {}
void QMI8658_update_if_needed() {}
void setAccODR(acc_odr_t) {}
void setGyroODR(gyro_odr_t) {}
void setAccScale(acc_scale_t) {}
void setGyroScale(gyro_scale_t) {}
void setAccLPF(lpf_t) {}
void setGyroLPF(lpf_t) {}
void setState(sensor_state_t) {}
void getRawReadings(int16_t*) {}
float getAccX()  { return 0; }
float getAccY()  { return 0; }
float getAccZ()  { return 0; }
float getGyroX() { return 0; }
float getGyroY() { return 0; }
float getGyroZ() { return 0; }
void getAccelerometer() {}
void getGyroscope() {}
