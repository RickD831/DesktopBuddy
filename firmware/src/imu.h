#ifndef IMU_H
#define IMU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QMI8658 accelerometer (system I2C bus, addr 0x6B). i2c_master_Init() first. */
bool imu_init(void);
bool imu_read(int16_t *ax, int16_t *ay, int16_t *az);  /* ~16384 LSB per g */

#ifdef __cplusplus
}
#endif

#endif
