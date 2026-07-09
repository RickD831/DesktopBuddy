#include "power_latch.h"
#include <Arduino.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#define TCA9554_ADDR   0x20        /* ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000 */
#define REG_OUTPUT     0x01
#define REG_CONFIG     0x03        /* bit=1: input, bit=0: output */
#define LATCH_BIT      0x40        /* expander pin 6 */
#define PWR_BTN_GPIO   GPIO_NUM_16 /* PWR button, high while pressed */
#define OFF_HOLD_MS    3000

static i2c_master_dev_handle_t exp_dev = NULL;
static bool armed = false;         /* boot press released at least once */
static uint32_t high_since = 0;

static bool reg_rmw(uint8_t reg, uint8_t set_mask, uint8_t clr_mask)
{
    uint8_t val = 0;
    if (i2c_master_transmit_receive(exp_dev, &reg, 1, &val, 1, 100) != ESP_OK)
        return false;
    val = (uint8_t)((val | set_mask) & (uint8_t)~clr_mask);
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(exp_dev, buf, 2, 100) == ESP_OK;
}

void power_latch_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(0, &bus) != ESP_OK || bus == NULL) return;

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = TCA9554_ADDR;
    cfg.scl_speed_hz = 300000;
    if (i2c_master_bus_add_device(bus, &cfg, &exp_dev) != ESP_OK) {
        exp_dev = NULL;
        return;
    }
    reg_rmw(REG_OUTPUT, LATCH_BIT, 0);   /* pin 6 high: hold power */
    reg_rmw(REG_CONFIG, 0, LATCH_BIT);   /* pin 6 as output */

    gpio_config_t io = {};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_INPUT;
    io.pin_bit_mask = (uint64_t)1 << PWR_BTN_GPIO;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
}

void power_latch_off(void)
{
    if (exp_dev) reg_rmw(REG_OUTPUT, 0, LATCH_BIT);
}

void power_latch_poll(void)
{
    if (exp_dev == NULL) return;
    int pressed = gpio_get_level(PWR_BTN_GPIO);
    uint32_t now = millis();
    if (!armed) {                 /* wait for the boot press to be released */
        if (!pressed) armed = true;
        return;
    }
    if (pressed) {
        if (high_since == 0) high_since = now;
        else if (now - high_since > OFF_HOLD_MS) {
            high_since = 0;
            power_latch_off();    /* lights out (battery only; no-op on USB) */
        }
    } else {
        high_since = 0;
    }
}
