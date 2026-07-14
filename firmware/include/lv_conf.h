/**
 * Minimal LVGL 9 configuration for Desktop Buddy.
 * Anything not set here falls back to LVGL's defaults (lv_conf_internal.h).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

#define LV_MEM_SIZE (96 * 1024U)

/* Fonts used by the buddy UI */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1

#define LV_USE_LOG 0

#endif /* LV_CONF_H */
