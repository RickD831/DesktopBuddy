#ifndef BUDDY_UI_H
#define BUDDY_UI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOOD_SLEEP = 0,   /* companion offline or PC idle */
    MOOD_IDLE,        /* connected, Claude not doing anything */
    MOOD_WORKING,     /* Claude is actively working */
    MOOD_WAITING,     /* Claude waiting on user (permission / question) */
    MOOD_HAPPY,       /* task finished */
    MOOD_ERROR,       /* something failed */
} buddy_mood_t;

typedef enum {
    NOTIFY_INFO = 0,
    NOTIFY_OK,
    NOTIFY_ERR,
} buddy_notify_kind_t;

/* All functions must be called with the LVGL lock held (lvgl_port_lock). */

void buddy_ui_init(void);

void buddy_set_mood(buddy_mood_t mood);

/* Any string may be NULL to leave it unchanged. cpu/ram: 0..100, -1 = unchanged. */
void buddy_set_status(int cpu, int ram, const char *clk, const char *date,
                      const char *headline, const char *msg, const char *proj);

void buddy_notify(buddy_notify_kind_t kind, const char *msg);

/* Claude usage gauge. pct: 0..100+ (-1 = unknown), strings may be NULL. */
void buddy_set_usage(int pct, const char *reset, const char *block, const char *day);

/* Weekly limits (official API): -1 = unknown, reset may be NULL. */
void buddy_set_weekly(int all_pct, int model_pct, const char *reset);

/* Battery gauge. pct 0..100, charging = on USB power, ble = data link is BLE. */
void buddy_set_battery(int pct, bool charging, bool ble);

/* Context window of the active session. pct -1 = unknown, txt e.g. "323k / 1.0M". */
void buddy_set_context(int pct, const char *txt);

/* Fired from LVGL task when the user long-presses the buddy's face. */
void buddy_set_pet_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif
