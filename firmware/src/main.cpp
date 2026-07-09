/* Claude Buddy — main firmware entry.
 *
 * Listens for newline-delimited JSON from the Windows companion app over
 * native USB CDC and drives the buddy UI. See README.md for the protocol.
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include "lvgl.h"
#include "lvgl_port.h"
#include "i2c_bsp.h"
#include "buddy_ui.h"
#include "ble_link.h"
#include "power_latch.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"

#define FW_VERSION "1.0.0"
#define COMPANION_TIMEOUT_MS 30000
#define HAPPY_HOLD_MS 6000

static char line_buf[512];
static size_t line_len = 0;
static uint32_t last_rx_ms = 0;
static bool companion_online = false;
static uint32_t happy_until_ms = 0;
static bool last_rx_was_ble = false;
static uint32_t last_batt_ms = 0;

#define BATT_ADC_PIN 4          /* ADC1_CH3, VBAT through a 1:3 divider */

/* resting LiPo voltage (mV) -> percent, linear between curve points */
static int batt_percent(int mv)
{
    static const int curve[][2] = {
        {4150, 100}, {4050, 90}, {3970, 80}, {3910, 70}, {3850, 60},
        {3800, 50}, {3760, 40}, {3720, 30}, {3670, 20}, {3600, 10}, {3500, 0},
    };
    if (mv >= curve[0][0]) return 100;
    for (int i = 1; i < 11; i++) {
        if (mv >= curve[i][0]) {
            int v0 = curve[i][0], p0 = curve[i][1];
            int v1 = curve[i - 1][0], p1 = curve[i - 1][1];
            return p0 + (mv - v0) * (p1 - p0) / (v1 - v0);
        }
    }
    return 0;
}

static void update_battery(void)
{
    uint32_t mv_sum = 0;
    for (int i = 0; i < 8; i++) mv_sum += analogReadMilliVolts(BATT_ADC_PIN);
    int vbat_mv = (int)(mv_sum / 8) * 3;
    /* USB power holds the rail at charge voltage; treat >4.3V as charging,
       and serial-link presence as the stronger "on USB" signal */
    bool charging = !last_rx_was_ble || vbat_mv > 4300;
    if (lvgl_port_lock(50)) {
        buddy_set_battery(batt_percent(vbat_mv), charging && companion_online,
                          last_rx_was_ble);
        lvgl_port_unlock();
    }
}

static void send_reply(const char *line)
{
    Serial.println(line);
    ble_link_send(line);
}

static void send_pet_event(void)
{
    send_reply("{\"t\":\"pet\"}");
}

static buddy_mood_t mood_from_state(const char *s)
{
    if (!s) return MOOD_IDLE;
    if (!strcmp(s, "working") || !strcmp(s, "thinking")) return MOOD_WORKING;
    if (!strcmp(s, "waiting")) return MOOD_WAITING;
    if (!strcmp(s, "done"))    return MOOD_HAPPY;
    if (!strcmp(s, "error"))   return MOOD_ERROR;
    if (!strcmp(s, "sleep") || !strcmp(s, "off")) return MOOD_SLEEP;
    return MOOD_IDLE;
}

static void handle_line(const char *line)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) return;

    const char *type = doc["t"] | "";

    if (!strcmp(type, "s")) { /* periodic status */
        int cpu = doc["cpu"] | -1;
        int ram = doc["ram"] | -1;
        const char *clk = doc["clk"] | (const char *)nullptr;
        const char *date = doc["date"] | (const char *)nullptr;
        const char *claude = doc["claude"] | (const char *)nullptr;
        const char *headline = doc["head"] | (const char *)nullptr;
        const char *msg = doc["msg"] | (const char *)nullptr;
        const char *proj = doc["proj"] | (const char *)nullptr;

        buddy_mood_t mood = mood_from_state(claude);
        if (mood == MOOD_HAPPY) happy_until_ms = millis() + HAPPY_HOLD_MS;
        /* let a recent "done" smile linger through idle updates */
        if (mood == MOOD_IDLE && millis() < happy_until_ms) mood = MOOD_HAPPY;

        if (lvgl_port_lock(100)) {
            buddy_set_mood(mood);
            buddy_set_status(cpu, ram, clk, date, headline, msg, proj);
            if (doc["use"].is<JsonObject>()) {
                JsonObject u = doc["use"];
                buddy_set_usage(u["pct"] | -1,
                                u["rst"] | (const char *)nullptr,
                                u["blk"] | (const char *)nullptr,
                                u["day"] | (const char *)nullptr);
                buddy_set_weekly(u["w"] | -1, u["wm"] | -1,
                                 u["wrst"] | (const char *)nullptr);
            }
            buddy_set_context(doc["ctx"] | -1,
                              doc["ctxt"] | (const char *)nullptr);
            lvgl_port_unlock();
        }
    } else if (!strcmp(type, "n")) { /* notification toast */
        const char *kind = doc["kind"] | "info";
        const char *msg = doc["msg"] | "";
        buddy_notify_kind_t k = NOTIFY_INFO;
        if (!strcmp(kind, "ok")) k = NOTIFY_OK;
        else if (!strcmp(kind, "err")) k = NOTIFY_ERR;
        if (lvgl_port_lock(100)) {
            buddy_notify(k, msg);
            lvgl_port_unlock();
        }
    } else if (!strcmp(type, "ping")) {
        send_reply("{\"t\":\"pong\",\"fw\":\"" FW_VERSION "\"}");
    }
}

void setup()
{
    Serial.begin(115200);
    i2c_master_Init();
    power_latch_init();   /* grab battery power before the PWR button is released */
    lvgl_port_init();
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

    if (lvgl_port_lock(-1)) {
        buddy_ui_init();
        buddy_set_pet_cb(send_pet_event);
        lvgl_port_unlock();
    }
    ble_link_init();
    Serial.println("{\"t\":\"hello\",\"fw\":\"" FW_VERSION "\"}");
}

void loop()
{
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (line_len > 0) {
                line_buf[line_len] = '\0';
                handle_line(line_buf);
                last_rx_ms = millis();
                companion_online = true;
                last_rx_was_ble = false;
                line_len = 0;
            }
        } else if (line_len < sizeof(line_buf) - 1) {
            line_buf[line_len++] = c;
        } else {
            line_len = 0; /* overlong line: discard */
        }
    }

    static char ble_buf[512];
    while (ble_link_read_line(ble_buf, sizeof(ble_buf)) > 0) {
        handle_line(ble_buf);
        last_rx_ms = millis();
        companion_online = true;
        last_rx_was_ble = true;
    }

    if (millis() - last_batt_ms > 5000) {
        last_batt_ms = millis();
        update_battery();
    }

    power_latch_poll();   /* hold PWR ~3s to power off on battery */

    if (companion_online && millis() - last_rx_ms > COMPANION_TIMEOUT_MS) {
        companion_online = false;
        if (lvgl_port_lock(100)) {
            buddy_set_mood(MOOD_SLEEP);
            buddy_set_status(-1, -1, nullptr, nullptr, "companion offline",
                             "is buddy_companion.py still running?", "");
            lvgl_port_unlock();
        }
    }

    delay(10);
}
