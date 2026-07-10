#include "imu.h"
#include "i2c_bsp.h"

#define REG_WHO_AM_I 0x00   /* reads 0x05 */
#define REG_CTRL1    0x02   /* bit6: register address auto-increment */
#define REG_CTRL2    0x03   /* accel full-scale + ODR */
#define REG_CTRL7    0x08   /* bit0: accel enable */
#define REG_AX_L     0x35   /* AX_L..AZ_H, little endian */

static bool imu_ok = false;

bool imu_init(void)
{
    uint8_t id = 0;
    if (i2c_read_buff(imu_dev_handle, REG_WHO_AM_I, &id, 1) != 0 || id != 0x05)
        return false;
    uint8_t v = 0x40;                      /* address auto-increment */
    i2c_write_buff(imu_dev_handle, REG_CTRL1, &v, 1);
    v = 0x06;                              /* ±2g, ~120 Hz */
    i2c_write_buff(imu_dev_handle, REG_CTRL2, &v, 1);
    v = 0x01;                              /* enable accelerometer */
    i2c_write_buff(imu_dev_handle, REG_CTRL7, &v, 1);
    imu_ok = true;
    return true;
}

bool imu_read(int16_t *ax, int16_t *ay, int16_t *az)
{
    if (!imu_ok) return false;
    uint8_t b[6];
    if (i2c_read_buff(imu_dev_handle, REG_AX_L, b, 6) != 0) return false;
    *ax = (int16_t)((b[1] << 8) | b[0]);
    *ay = (int16_t)((b[3] << 8) | b[2]);
    *az = (int16_t)((b[5] << 8) | b[4]);
    return true;
}
