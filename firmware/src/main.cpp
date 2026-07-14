/* Desktop Buddy — main firmware entry.
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
#include "imu.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"

#define FW_VERSION "1.0.0"
#define COMPANION_TIMEOUT_MS 30000
#define HAPPY_HOLD_MS 6000

static char line_buf[768];
static size_t line_len = 0;
static uint32_t last_rx_ms = 0;
static bool companion_online = false;
static uint32_t happy_until_ms = 0;
static bool last_rx_was_ble = false;
static uint32_t last_batt_ms = 0;

#define BATT_ADC_PIN 4          /* ADC1_CH3, VBAT through a 1:3 divider */

/* album art: 120x120 RGB565 streamed in base64 chunks of 510 raw bytes */
#define ART_W 120
#define ART_H 120
#define ART_BYTES (ART_W * ART_H * 2)
#define ART_RAW_CHUNK 480
static uint8_t *art_buf = NULL;
static uint32_t art_seen_mask_ok = 0;   /* count of chunks received in order */

static void send_reply(const char *line);

static void handle_art(JsonObject a)
{
    int w = a["w"] | 0, h = a["h"] | 0;
    int seq = a["seq"] | -1, n = a["n"] | 0;
    const char *d = a["d"] | "";
    if (w != ART_W || h != ART_H || seq < 0 || n <= 0 || !d[0]) return;
    if (art_buf == NULL) {
        art_buf = (uint8_t *)heap_caps_malloc(ART_BYTES, MALLOC_CAP_SPIRAM);
        if (art_buf == NULL) return;
    }
    size_t offset = (size_t)seq * ART_RAW_CHUNK;
    if (offset >= ART_BYTES) return;
    size_t out_len = 0;
    if (mbedtls_base64_decode(art_buf + offset, ART_BYTES - offset, &out_len,
                              (const unsigned char *)d, strlen(d)) != 0)
        return;
    if (seq == 0) art_seen_mask_ok = 0;
    if ((int)art_seen_mask_ok == seq) art_seen_mask_ok++;
    if (seq == n - 1) {
        if ((int)art_seen_mask_ok == n) {
            if (lvgl_port_lock(100)) {
                buddy_media_art_ready(art_buf, ART_W, ART_H);
                lvgl_port_unlock();
            }
            send_reply("{\"t\":\"artok\"}");
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "{\"t\":\"artdrop\",\"got\":%u,\"n\":%d}", art_seen_mask_ok, n);
            send_reply(buf);
        }
    }
}

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

/* orientation from gravity: which way is the bar display standing? */
static int imu_filt[3] = {0, 0, 0};
static uint32_t last_imu_ms = 0;

static void update_orientation(void)
{
    int16_t ax, ay, az;
    if (!imu_read(&ax, &ay, &az)) return;
    imu_filt[0] = (imu_filt[0] * 7 + ax * 3) / 10;
    imu_filt[1] = (imu_filt[1] * 7 + ay * 3) / 10;
    imu_filt[2] = (imu_filt[2] * 7 + az * 3) / 10;

    /* X is the panel's long axis (measured: landscape stand reads x~0,
       gravity split between y and z from the stand's back-lean).
       Y's sign says which way up the landscape is. */
    const int TH = 11500;   /* ~0.7 g of ~16384 LSB/g */
    int want = 0;
    if (imu_filt[0] > TH) want = 1;
    else if (imu_filt[0] < -TH) want = 2;
    else if (imu_filt[1] > 8000) want = 3;   /* landscape, upside down */

    static int pend = 0, cnt = 0;
    if (want == pend) {
        if (cnt < 4 && ++cnt == 4) {
            if (lvgl_port_lock(100)) {
                buddy_set_orientation(want);
                lvgl_port_unlock();
            }
        }
    } else {
        pend = want;
        cnt = 0;
    }
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

static void send_media_cmd(const char *cmd)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"t\":\"mc\",\"cmd\":\"%s\"}", cmd);
    send_reply(buf);
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
            if (doc["cdx"].is<JsonObject>()) {
                JsonObject c = doc["cdx"];
                buddy_set_codex(c["ctx"] | -1, c["ctxt"] | (const char *)nullptr,
                                 c["pct"] | -1, c["rst"] | (const char *)nullptr);
                buddy_mood_t cmood = mood_from_state(c["state"] | (const char *)nullptr);
                buddy_set_codex_status(cmood, c["head"] | (const char *)nullptr,
                                       c["msg"] | (const char *)nullptr,
                                       c["proj"] | (const char *)nullptr);
            }
            if (doc["med"].is<JsonObject>()) {
                JsonObject m = doc["med"];
                const char *st = m["st"] | "";
                buddy_set_media(m["t"] | (const char *)nullptr,
                                m["a"] | (const char *)nullptr,
                                m["app"] | (const char *)nullptr,
                                !strcmp(st, "playing") ? 1 : 0,
                                m["pos"] | 0, m["dur"] | 0);
            } else {
                buddy_set_media(nullptr, nullptr, nullptr, -1, 0, 0);
            }
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
    } else if (!strcmp(type, "art")) {
        handle_art(doc.as<JsonObject>());
    } else if (!strcmp(type, "ping")) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "{\"t\":\"pong\",\"fw\":\"" FW_VERSION "\",\"imu\":[%d,%d,%d]}",
                 imu_filt[0], imu_filt[1], imu_filt[2]);
        send_reply(buf);
    }
}

void setup()
{
    Serial.setRxBufferSize(16384);   /* album-art bursts overflow the 256B default */
    Serial.begin(115200);
    i2c_master_Init();
    power_latch_init();   /* grab battery power before the PWR button is released */
    imu_init();
    lvgl_port_init();
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

    if (lvgl_port_lock(-1)) {
        buddy_ui_init();
        buddy_set_pet_cb(send_pet_event);
        buddy_set_media_cmd_cb(send_media_cmd);
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

    static char ble_buf[768];   /* must fit art chunk lines (~700 chars) */
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

    if (millis() - last_imu_ms > 250) {
        last_imu_ms = millis();
        update_orientation();
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

    delay(5);
}
