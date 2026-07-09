#ifndef USER_CONFIG_H
#define USER_CONFIG_H

// Pin map and panel constants for Waveshare ESP32-S3-Touch-LCD-3.49
// (values taken from the vendor demo: waveshareteam/ESP32-S3-Touch-LCD-3.49)

// spi & i2c handle
#define LCD_HOST SPI3_HOST

// touch I2C port
#define Touch_SCL_NUM (GPIO_NUM_18)
#define Touch_SDA_NUM (GPIO_NUM_17)

// system I2C (RTC + IMU)
#define ESP_SCL_NUM (GPIO_NUM_48)
#define ESP_SDA_NUM (GPIO_NUM_47)

// QSPI display
#define EXAMPLE_PIN_NUM_LCD_CS     (GPIO_NUM_9)
#define EXAMPLE_PIN_NUM_LCD_PCLK   (GPIO_NUM_10)
#define EXAMPLE_PIN_NUM_LCD_DATA0  (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_LCD_DATA1  (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_DATA2  (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_DATA3  (GPIO_NUM_14)
#define EXAMPLE_PIN_NUM_LCD_RST    (GPIO_NUM_21)
#define EXAMPLE_PIN_NUM_BK_LIGHT   (GPIO_NUM_8)

#define I2C_TOUCH_ADDR                    0x3b
#define EXAMPLE_PIN_NUM_TOUCH_RST         (-1)
#define EXAMPLE_PIN_NUM_TOUCH_INT         (-1)

#define LVGL_TICK_PERIOD_MS    5
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 5
#define LVGL_TASK_STACK_SIZE   (8 * 1024)
#define LVGL_TASK_PRIORITY     2

/*ADDR*/
#define EXAMPLE_RTC_ADDR 0x51
#define EXAMPLE_IMU_ADDR 0x6b

#define USER_DISP_ROT_90    1
#define USER_DISP_ROT_NONO  0
// Landscape: 640 wide x 172 tall (software rotation in the flush callback)
#define Rotated USER_DISP_ROT_90

#define EXAMPLE_LCD_H_RES 172
#define EXAMPLE_LCD_V_RES 640

#define LCD_NOROT_HRES     172
#define LCD_NOROT_VRES     640
#define LVGL_DMA_BUFF_LEN (LCD_NOROT_HRES * 64 * 2)
#define LVGL_SPIRAM_BUFF_LEN (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2)

#endif
