/* Claude Buddy UI — 640x172 landscape, LVGL 9.
 *
 * Screen 0 "buddy":     Claude/Codex face(s) | status text | clock +
 *                       context/usage gauges (see update_dual_layout())
 * Screen 1 "zen":       big clock
 * Screen 2 "use":       Claude usage breakdown (5h/weekly bars)
 * Screen 3 "cdxu":      Codex usage breakdown (single bar)
 * Screen 4 "stats":     CPU/RAM
 * Screen 5 "media":     now playing
 * Tap anywhere (outside the face) cycles screens; long-press the face to pet.
 */
#include "buddy_ui.h"
#include "lvgl.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include <stdio.h>
#include <string.h>
#include <initializer_list>

/* ---------- palette ---------- */
#define C_BG        lv_color_hex(0x0F0F1A)
#define C_PANEL     lv_color_hex(0x1A1A2E)
#define C_EYE       lv_color_hex(0xF2EFEA)
#define C_PUPIL     lv_color_hex(0x14141F)
#define C_TEXT      lv_color_hex(0xC9C7C1)
#define C_DIM       lv_color_hex(0x6E6C78)
#define C_BAR_BG    lv_color_hex(0x2A2A3E)

#define C_IDLE      lv_color_hex(0x9A98A8)
#define C_WORKING   lv_color_hex(0xD97757)   /* Claude orange */
#define C_WAITING   lv_color_hex(0xE5A84B)
#define C_HAPPY     lv_color_hex(0x7BC47F)
#define C_ERROR     lv_color_hex(0xE5484D)
#define C_SLEEP     lv_color_hex(0x4A4E69)
#define C_CODEX     lv_color_hex(0x4A90D9)   /* Codex identity blue, not mood-reactive */

#define SCR_W 640
#define SCR_H 172
#define FACE_W 205

static buddy_mood_t cur_mood = MOOD_SLEEP;
static void (*pet_cb)(void) = NULL;

static lv_obj_t *scr_buddy;
static lv_obj_t *scr_zen;
static int cur_screen = 0;

/* face parts */
static lv_obj_t *face_cont;
static lv_obj_t *eye_l, *eye_r;
static lv_obj_t *pupil_l, *pupil_r;
static lv_obj_t *brow_l, *brow_r;
static lv_obj_t *mouth_smile, *mouth_frown, *mouth_o, *mouth_flat;
static lv_obj_t *zzz_lbl;
static int eye_base_h = 8;
static bool blink_enabled = false;

/* Codex's dedicated (reduced-mood) face — deliberately simpler than the
 * Claude face above: only smile/flat mouths, no brows/frown/zzz. */
static buddy_mood_t cur_mood_codex = MOOD_SLEEP;   /* SLEEP here just means "no data yet" */
static lv_obj_t *cdx_face_cont;
static lv_obj_t *cdx_eye_l, *cdx_eye_r;
static lv_obj_t *cdx_pupil_l, *cdx_pupil_r;
static lv_obj_t *cdx_mouth_smile, *cdx_mouth_flat;
static lv_obj_t *cdx_headline_lbl, *cdx_msg_lbl;

/* status widgets (buddy screen) */
static lv_obj_t *headline_lbl, *msg_lbl, *proj_lbl;
static lv_obj_t *dots[3];
static lv_obj_t *clk_lbl, *date_lbl;
static lv_obj_t *cpu_bar, *ram_bar, *cpu_lbl, *ram_lbl;

/* zen screen widgets */
static lv_obj_t *zen_clk_lbl, *zen_date_lbl, *zen_foot_lbl;

/* usage widgets (buddy screen mini-gauge + dedicated screen) */
static lv_obj_t *scr_use;
static lv_obj_t *use_mini_bar, *use_mini_lbl;
static lv_obj_t *use_pct_lbl, *use_bar, *use_reset_lbl, *use_detail_lbl;
static lv_obj_t *week_all_bar, *week_all_lbl, *week_mdl_bar, *week_mdl_lbl;

/* battery + link indicator (one label per screen) */
static lv_obj_t *batt_lbl, *zen_batt_lbl, *use_batt_lbl, *stats_batt_lbl;
static lv_obj_t *use_title_lbl, *stats_title_lbl;

/* PC stats screen + context gauge on the buddy screen */
static lv_obj_t *scr_stats;
static lv_obj_t *ctx_bar, *ctx_lbl;

/* Codex CLI mini-gauges on the buddy screen (mirrors ctx_bar/use_mini_bar) */
static lv_obj_t *cdx_ctx_bar, *cdx_ctx_lbl, *cdx_use_bar, *cdx_use_lbl;

/* Codex CLI dedicated usage screen (mirrors scr_use, but a single bar —
 * Codex only reports one rate-limit window, not a 5-hour + weekly pair) */
static lv_obj_t *scr_cdxu;
static lv_obj_t *cdxu_title_lbl, *cdxu_pct_lbl, *cdxu_detail_lbl, *cdxu_reset_lbl, *cdxu_bar, *cdxu_batt_lbl;

/* now-playing screen */
static lv_obj_t *scr_media;
static lv_obj_t *med_title_lbl, *med_artist_lbl, *med_app_lbl;
static lv_obj_t *med_bar, *med_time_lbl, *med_batt_lbl;
static lv_obj_t *med_btn_prev, *med_btn_play, *med_btn_next, *med_play_icon;
static lv_obj_t *med_line_lbl;          /* now-playing line on the buddy screen */
static lv_obj_t *dancer;                /* solo dancer with headphones */
static lv_obj_t *dc_eye_l, *dc_eye_r;   /* its eyes (for blinking) */
static void (*media_cmd_cb)(const char *cmd) = NULL;
static int med_pos = 0, med_dur = 0, med_playing = -1;
static char med_last_title[64] = "";
static bool art_new_track = false;      /* interlude the next completed art */

/* album art (pixels live in main.cpp's PSRAM buffer) */
static lv_obj_t *art_img_buddy, *art_img_media;
static lv_image_dsc_t art_dsc;
static bool art_valid = false;

static void buddy_gesture_cb(lv_event_t *e);
static void art_peek(uint32_t ms);

/* notification overlay */
static lv_obj_t *toast, *toast_icon, *toast_lbl;

static int last_cpu = 0, last_ram = 0;

/* ---------- small helpers ---------- */

static void anim_height_cb(void *obj, int32_t v) { lv_obj_set_height((lv_obj_t *)obj, v); }
static void anim_opa_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }

static lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *make_hbar(lv_obj_t *parent, int y);
static void apply_codex_mood(buddy_mood_t m);

static lv_color_t mood_accent(buddy_mood_t m)
{
    switch (m) {
        case MOOD_WORKING: return C_WORKING;
        case MOOD_WAITING: return C_WAITING;
        case MOOD_HAPPY:   return C_HAPPY;
        case MOOD_ERROR:   return C_ERROR;
        case MOOD_IDLE:    return C_IDLE;
        default:           return C_SLEEP;
    }
}

/* ---------- face ---------- */

static void set_pupils(int dx, int dy, bool visible)
{
    if (visible) {
        lv_obj_clear_flag(pupil_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(pupil_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(pupil_l, LV_ALIGN_CENTER, dx, dy);
        lv_obj_align(pupil_r, LV_ALIGN_CENTER, dx, dy);
    } else {
        lv_obj_add_flag(pupil_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pupil_r, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_mouth(lv_obj_t *which)
{
    lv_obj_add_flag(mouth_smile, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mouth_frown, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mouth_o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mouth_flat, LV_OBJ_FLAG_HIDDEN);
    if (which) lv_obj_clear_flag(which, LV_OBJ_FLAG_HIDDEN);
}

static void set_eyes(int w, int h)
{
    eye_base_h = h;
    lv_obj_set_size(eye_l, w, h);
    lv_obj_set_size(eye_r, w, h);
}

static void blink_timer_cb(lv_timer_t *t)
{
    if (!blink_enabled) return;
    lv_obj_t *eyes[2] = { eye_l, eye_r };
    for (int i = 0; i < 2; i++) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, eyes[i]);
        lv_anim_set_exec_cb(&a, anim_height_cb);
        lv_anim_set_values(&a, eye_base_h, 5);
        lv_anim_set_duration(&a, 80);
        lv_anim_set_playback_duration(&a, 100);
        lv_anim_start(&a);
    }
    lv_timer_set_period(t, 2200 + lv_rand(0, 2800));
}

static void wander_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (cur_mood == MOOD_IDLE) {
        if (lv_rand(0, 3) == 0)
            set_pupils((int)lv_rand(0, 16) - 8, (int)lv_rand(0, 12) - 6, true);
    } else if (cur_mood == MOOD_WORKING) {
        /* focused: pupils up-right with a little jitter, like it's reading */
        set_pupils(5 + (int)lv_rand(0, 4), -5 + (int)lv_rand(0, 2), true);
    }
}

static void dots_set_visible(bool vis)
{
    for (int i = 0; i < 3; i++) {
        if (vis) lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_mood(buddy_mood_t m)
{
    cur_mood = m;
    lv_color_t accent = mood_accent(m);

    lv_obj_set_style_text_color(headline_lbl, accent, 0);
    lv_obj_set_style_border_color(face_cont, accent, 0);
    for (int i = 0; i < 3; i++) lv_obj_set_style_bg_color(dots[i], accent, 0);

    lv_obj_add_flag(zzz_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(brow_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(brow_r, LV_OBJ_FLAG_HIDDEN);
    dots_set_visible(false);
    blink_enabled = false;

    switch (m) {
        case MOOD_SLEEP:
            set_eyes(44, 8);
            set_pupils(0, 0, false);
            show_mouth(mouth_flat);
            lv_obj_clear_flag(zzz_lbl, LV_OBJ_FLAG_HIDDEN);
            setUpduty(LCD_PWM_MODE_150);
            return; /* keep it dim */
        case MOOD_IDLE:
            set_eyes(36, 56);
            set_pupils(0, 0, true);
            show_mouth(mouth_smile);
            lv_arc_set_bg_angles(mouth_smile, 40, 140);
            blink_enabled = true;
            break;
        case MOOD_WORKING:
            set_eyes(36, 44);
            set_pupils(6, -5, true);
            show_mouth(mouth_flat);
            dots_set_visible(true);
            blink_enabled = true;
            break;
        case MOOD_WAITING:
            set_eyes(40, 62);
            set_pupils(0, 0, true);
            show_mouth(mouth_o);
            blink_enabled = true;
            break;
        case MOOD_HAPPY:
            set_eyes(40, 16);
            set_pupils(0, 0, false);
            show_mouth(mouth_smile);
            lv_arc_set_bg_angles(mouth_smile, 20, 160);
            break;
        case MOOD_ERROR:
            set_eyes(36, 46);
            set_pupils(0, 2, true);
            show_mouth(mouth_frown);
            lv_obj_clear_flag(brow_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(brow_r, LV_OBJ_FLAG_HIDDEN);
            break;
    }
    setUpduty(LCD_PWM_MODE_255);
}

/* ---------- interactions ---------- */

static void restore_after_pet_cb(lv_timer_t *t)
{
    buddy_mood_t prev = (buddy_mood_t)(intptr_t)lv_timer_get_user_data(t);
    apply_mood(prev);
}

static void face_long_press_cb(lv_event_t *e)
{
    (void)e;
    buddy_mood_t prev = (cur_mood == MOOD_HAPPY) ? MOOD_IDLE : cur_mood;
    apply_mood(MOOD_HAPPY);
    lv_label_set_text(headline_lbl, "hehe, thanks!");
    lv_timer_t *t = lv_timer_create(restore_after_pet_cb, 2500, (void *)(intptr_t)prev);
    lv_timer_set_repeat_count(t, 1);
    if (pet_cb) pet_cb();
}

static void screen_click_cb(lv_event_t *e)
{
    (void)e;
    cur_screen = (cur_screen + 1) % 6;
    lv_obj_t *screens[6] = { scr_buddy, scr_zen, scr_use, scr_cdxu, scr_stats, scr_media };
    lv_obj_t *next = screens[cur_screen];
    lv_screen_load_anim(next, LV_SCR_LOAD_ANIM_FADE_IN, 180, 0, false);
    /* touching the screen also wakes a sleeping buddy's backlight briefly */
    if (cur_mood != MOOD_SLEEP) setUpduty(LCD_PWM_MODE_255);
}

/* ---------- notification toast ---------- */

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

void buddy_notify(buddy_notify_kind_t kind, const char *msg)
{
    lv_color_t c = (kind == NOTIFY_OK) ? C_HAPPY : (kind == NOTIFY_ERR) ? C_ERROR : C_WAITING;
    const char *icon = (kind == NOTIFY_OK) ? LV_SYMBOL_OK : (kind == NOTIFY_ERR) ? LV_SYMBOL_WARNING : LV_SYMBOL_BELL;
    lv_obj_set_style_border_color(toast, c, 0);
    lv_obj_set_style_text_color(toast_icon, c, 0);
    lv_label_set_text(toast_icon, icon);
    lv_label_set_text(toast_lbl, msg ? msg : "");
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(toast_hide_cb, 4000, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* ---------- Claude/Codex dual-face layout ---------- */

/* Single-engine ("normal") geometry per orientation, as fixed formulas —
 * the exact numbers build_buddy_screen()/layout_apply() use for the solo
 * face and its status text. update_dual_layout() computes from these fresh
 * every call rather than reading back a live widget's current position:
 * since update_dual_layout() is also what *mutates* those widgets (shrinking
 * them for the stacked view), reading its own prior output back as the
 * "normal" baseline would compound the shrink on every subsequent call. */
static bool g_portrait = false;

static void normal_face_geom(int *x, int *y, int *w, int *h)
{
    if (g_portrait) { *x = 6; *y = 6; *w = 160; *h = 150; }
    else            { *x = 7; *y = 7; *w = FACE_W - 14; *h = SCR_H - 14; }
}

static void normal_text_geom(int *hx, int *hy, int *mx, int *my, int *px, int *py)
{
    if (g_portrait) {
        *hx = 10; *hy = 168; *mx = 10; *my = 198; *px = 10; *py = 224;
    } else {
        *hx = FACE_W + 18; *hy = 14; *mx = FACE_W + 18; *my = 46; *px = FACE_W + 18; *py = 70;
    }
}

/* Canonical (full-size) eye/pupil/mouth geometry per mood — the same numbers
 * apply_mood()/apply_codex_mood() set directly. Scaling always starts from
 * these fixed values (never from a widget's current, possibly-already-
 * scaled size), so repeated calls in dual mode don't compound the shrink. */
static void claude_mood_geom(buddy_mood_t m, int *eye_w, int *eye_h, int *pdx, int *pdy, bool *pvis,
                              lv_obj_t **mouth, int *mw, int *mh, int *mdy)
{
    switch (m) {
        case MOOD_WAITING:
            *eye_w = 40; *eye_h = 62; *pdx = 0; *pdy = 0; *pvis = true;
            *mouth = mouth_o; *mw = 20; *mh = 20; *mdy = 44;
            break;
        case MOOD_HAPPY:
            *eye_w = 40; *eye_h = 16; *pdx = 0; *pdy = 0; *pvis = false;
            lv_arc_set_bg_angles(mouth_smile, 20, 160);
            *mouth = mouth_smile; *mw = 56; *mh = 56; *mdy = 32;
            break;
        case MOOD_ERROR:
            *eye_w = 36; *eye_h = 46; *pdx = 0; *pdy = 2; *pvis = true;
            *mouth = mouth_frown; *mw = 56; *mh = 56; *mdy = 72;
            break;
        case MOOD_SLEEP:
            *eye_w = 44; *eye_h = 8; *pdx = 0; *pdy = 0; *pvis = false;
            *mouth = mouth_flat; *mw = 34; *mh = 6; *mdy = 54;
            break;
        case MOOD_WORKING:
            *eye_w = 36; *eye_h = 44; *pdx = 6; *pdy = -5; *pvis = true;
            *mouth = mouth_flat; *mw = 34; *mh = 6; *mdy = 54;
            break;
        case MOOD_IDLE:
        default:
            *eye_w = 36; *eye_h = 56; *pdx = 0; *pdy = 0; *pvis = true;
            lv_arc_set_bg_angles(mouth_smile, 40, 140);
            *mouth = mouth_smile; *mw = 56; *mh = 56; *mdy = 32;
            break;
    }
}

static void codex_mood_geom(buddy_mood_t m, int *eye_w, int *eye_h, int *pdx, int *pdy, bool *pvis,
                             lv_obj_t **mouth, int *mw, int *mh, int *mdy)
{
    if (m == MOOD_WORKING) {
        *eye_w = 36; *eye_h = 44; *pdx = 6; *pdy = -5; *pvis = true;
        *mouth = cdx_mouth_flat; *mw = 34; *mh = 6; *mdy = 54;
    } else if (m == MOOD_HAPPY) {
        /* short, squinty eyes so they don't overlap the wider smile */
        *eye_w = 40; *eye_h = 16; *pdx = 0; *pdy = 0; *pvis = false;
        lv_arc_set_bg_angles(cdx_mouth_smile, 20, 160);
        *mouth = cdx_mouth_smile; *mw = 56; *mh = 56; *mdy = 32;
    } else {
        *eye_w = 36; *eye_h = 56; *pdx = 0; *pdy = 0; *pvis = true;
        lv_arc_set_bg_angles(cdx_mouth_smile, 40, 140);
        *mouth = cdx_mouth_smile; *mw = 56; *mh = 56; *mdy = 32;
    }
}

static void scale_face_shapes(lv_obj_t *eye_l_, lv_obj_t *eye_r_, lv_obj_t *pupil_l_, lv_obj_t *pupil_r_,
                               int eye_w, int eye_h, int pdx, int pdy, bool pvis,
                               lv_obj_t *mouth, int mw, int mh, int mdy, float s)
{
    lv_obj_set_size(eye_l_, (int)(eye_w * s + 0.5f), (int)(eye_h * s + 0.5f));
    lv_obj_set_size(eye_r_, (int)(eye_w * s + 0.5f), (int)(eye_h * s + 0.5f));
    lv_obj_align(eye_l_, LV_ALIGN_CENTER, (int)(-38 * s), (int)(-16 * s));
    lv_obj_align(eye_r_, LV_ALIGN_CENTER, (int)(38 * s), (int)(-16 * s));
    if (pvis) {
        lv_obj_clear_flag(pupil_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(pupil_r_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(pupil_l_, (int)(14 * s + 0.5f), (int)(14 * s + 0.5f));
        lv_obj_set_size(pupil_r_, (int)(14 * s + 0.5f), (int)(14 * s + 0.5f));
        lv_obj_align(pupil_l_, LV_ALIGN_CENTER, (int)(pdx * s), (int)(pdy * s));
        lv_obj_align(pupil_r_, LV_ALIGN_CENTER, (int)(pdx * s), (int)(pdy * s));
    } else {
        lv_obj_add_flag(pupil_l_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pupil_r_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_size(mouth, (int)(mw * s + 0.5f), (int)(mh * s + 0.5f));
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, (int)(mdy * s));
}

/* Arbitrates screen space between Claude's and Codex's faces: whichever is
 * idle gets out of the way so the active one uses the full face slot; if
 * both are busy at once they stack (shrunk) within that same footprint. */
static void update_dual_layout(void)
{
    bool claude_active = (cur_mood == MOOD_WORKING || cur_mood == MOOD_WAITING || cur_mood == MOOD_HAPPY);
    bool codex_active = (cur_mood_codex == MOOD_WORKING || cur_mood_codex == MOOD_HAPPY);

    int fx, fy, fw, fh, hx, hy, mx, my, px, py;
    normal_face_geom(&fx, &fy, &fw, &fh);
    normal_text_geom(&hx, &hy, &mx, &my, &px, &py);

    if (claude_active && codex_active) {
        int half = (fh - 4) / 2;
        lv_obj_clear_flag(face_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(face_cont, fw, half);
        lv_obj_set_pos(face_cont, fx, fy);
        lv_obj_clear_flag(cdx_face_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(cdx_face_cont, fw, half);
        lv_obj_set_pos(cdx_face_cont, fx, fy + half + 4);

        lv_obj_clear_flag(headline_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(proj_lbl, LV_OBJ_FLAG_HIDDEN);  /* drop it, cramped when stacked */
        lv_obj_set_pos(headline_lbl, hx, fy);
        lv_obj_set_pos(msg_lbl, mx, fy + 20);

        lv_obj_clear_flag(cdx_headline_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cdx_msg_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(cdx_headline_lbl, hx, fy + half + 4);
        lv_obj_set_pos(cdx_msg_lbl, mx, fy + half + 24);

        /* the containers shrank; shrink the eyes/pupils/mouth to match
         * instead of letting them overflow the smaller box */
        float s = (float)half / (float)fh;
        int ew, eh, pdx, pdy, mw, mh, mdy; bool pvis; lv_obj_t *mouth;
        claude_mood_geom(cur_mood, &ew, &eh, &pdx, &pdy, &pvis, &mouth, &mw, &mh, &mdy);
        scale_face_shapes(eye_l, eye_r, pupil_l, pupil_r, ew, eh, pdx, pdy, pvis, mouth, mw, mh, mdy, s);
        eye_base_h = (int)(eh * s + 0.5f);   /* keep blinking from popping back to full size */
        codex_mood_geom(cur_mood_codex, &ew, &eh, &pdx, &pdy, &pvis, &mouth, &mw, &mh, &mdy);
        scale_face_shapes(cdx_eye_l, cdx_eye_r, cdx_pupil_l, cdx_pupil_r, ew, eh, pdx, pdy, pvis, mouth, mw, mh, mdy, s);
    } else if (codex_active) {
        lv_obj_add_flag(face_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(headline_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(proj_lbl, LV_OBJ_FLAG_HIDDEN);
        dots_set_visible(false);

        lv_obj_clear_flag(cdx_face_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(cdx_face_cont, fw, fh);
        lv_obj_set_pos(cdx_face_cont, fx, fy);
        lv_obj_clear_flag(cdx_headline_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cdx_msg_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(cdx_headline_lbl, hx, hy);
        lv_obj_set_pos(cdx_msg_lbl, mx, my);
        /* Force full-size geometry unconditionally. apply_codex_mood() only
         * fires on a mood *change* and only touches size, never alignment —
         * scale_face_shapes() in the dual branch above moves eyes/mouth
         * inward as well as shrinking them, and nothing else ever moves
         * them back out. Re-running scale_face_shapes at s=1.0 restores
         * both position and size regardless of how we got here. */
        apply_codex_mood(cur_mood_codex);
        {
            int ew, eh, pdx, pdy, mw, mh, mdy; bool pvis; lv_obj_t *mouth;
            codex_mood_geom(cur_mood_codex, &ew, &eh, &pdx, &pdy, &pvis, &mouth, &mw, &mh, &mdy);
            scale_face_shapes(cdx_eye_l, cdx_eye_r, cdx_pupil_l, cdx_pupil_r,
                              ew, eh, pdx, pdy, pvis, mouth, mw, mh, mdy, 1.0f);
        }
    } else {
        lv_obj_clear_flag(face_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(face_cont, fw, fh);
        lv_obj_set_pos(face_cont, fx, fy);
        lv_obj_clear_flag(headline_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(msg_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(proj_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(headline_lbl, hx, hy);
        lv_obj_set_pos(msg_lbl, mx, my);
        lv_obj_set_pos(proj_lbl, px, py);
        /* same reasoning as the Codex branch above, for Claude's face —
         * apply_mood() alone only restores size, not alignment */
        apply_mood(cur_mood);
        {
            int ew, eh, pdx, pdy, mw, mh, mdy; bool pvis; lv_obj_t *mouth;
            claude_mood_geom(cur_mood, &ew, &eh, &pdx, &pdy, &pvis, &mouth, &mw, &mh, &mdy);
            scale_face_shapes(eye_l, eye_r, pupil_l, pupil_r,
                              ew, eh, pdx, pdy, pvis, mouth, mw, mh, mdy, 1.0f);
            eye_base_h = eh;
        }

        lv_obj_add_flag(cdx_face_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cdx_headline_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cdx_msg_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------- public API ---------- */

void buddy_set_pet_cb(void (*cb)(void)) { pet_cb = cb; }

void buddy_set_mood(buddy_mood_t mood)
{
    if (mood != cur_mood) apply_mood(mood);
    update_dual_layout();
}

void buddy_set_status(int cpu, int ram, const char *clk, const char *date,
                      const char *headline, const char *msg, const char *proj)
{
    char buf[32];
    if (cpu >= 0) {
        last_cpu = cpu;
        lv_bar_set_value(cpu_bar, cpu, LV_ANIM_ON);
        snprintf(buf, sizeof(buf), "CPU  %d%%", cpu);
        lv_label_set_text(cpu_lbl, buf);
        lv_obj_set_style_bg_color(cpu_bar, cpu > 85 ? C_ERROR : C_WORKING, LV_PART_INDICATOR);
    }
    if (ram >= 0) {
        last_ram = ram;
        lv_bar_set_value(ram_bar, ram, LV_ANIM_ON);
        snprintf(buf, sizeof(buf), "RAM  %d%%", ram);
        lv_label_set_text(ram_lbl, buf);
        lv_obj_set_style_bg_color(ram_bar, ram > 90 ? C_ERROR : C_IDLE, LV_PART_INDICATOR);
    }
    if (cpu >= 0 || ram >= 0) {
        snprintf(buf, sizeof(buf), "CPU %d%%   RAM %d%%", last_cpu, last_ram);
        lv_label_set_text(zen_foot_lbl, buf);
    }
    if (clk) {
        lv_label_set_text(clk_lbl, clk);
        lv_label_set_text(zen_clk_lbl, clk);
    }
    if (date) {
        lv_label_set_text(date_lbl, date);
        lv_label_set_text(zen_date_lbl, date);
    }
    if (headline) lv_label_set_text(headline_lbl, headline);
    if (msg) lv_label_set_text(msg_lbl, msg);
    if (proj) lv_label_set_text(proj_lbl, proj);
}

/* ---------- construction ---------- */

static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_click_cb, LV_EVENT_CLICKED, NULL);
    return scr;
}

static void build_face(lv_obj_t *parent)
{
    face_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(face_cont);
    lv_obj_set_size(face_cont, FACE_W - 14, SCR_H - 14);
    lv_obj_set_pos(face_cont, 7, 7);
    lv_obj_set_style_bg_color(face_cont, C_PANEL, 0);
    lv_obj_set_style_bg_opa(face_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(face_cont, 16, 0);
    lv_obj_set_style_border_width(face_cont, 2, 0);
    lv_obj_set_style_border_color(face_cont, C_SLEEP, 0);
    lv_obj_clear_flag(face_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(face_cont, face_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* eyes: rounded rects that change height with mood/blinks */
    eye_l = make_box(face_cont);
    eye_r = make_box(face_cont);
    for (lv_obj_t *e : { eye_l, eye_r }) {
        lv_obj_set_style_bg_color(e, C_EYE, 0);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(e, LV_RADIUS_CIRCLE, 0);
    }
    lv_obj_set_size(eye_l, 36, 56);
    lv_obj_set_size(eye_r, 36, 56);
    lv_obj_align(eye_l, LV_ALIGN_CENTER, -38, -16);
    lv_obj_align(eye_r, LV_ALIGN_CENTER, 38, -16);

    pupil_l = make_box(eye_l);
    pupil_r = make_box(eye_r);
    for (lv_obj_t *p : { pupil_l, pupil_r }) {
        lv_obj_set_size(p, 14, 14);
        lv_obj_set_style_bg_color(p, C_PUPIL, 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(p, LV_ALIGN_CENTER, 0, 0);
    }

    /* angry eyebrows (error mood) */
    brow_l = make_box(face_cont);
    brow_r = make_box(face_cont);
    for (lv_obj_t *b : { brow_l, brow_r }) {
        lv_obj_set_size(b, 34, 6);
        lv_obj_set_style_bg_color(b, C_EYE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(brow_l, LV_ALIGN_CENTER, -38, -54);
    lv_obj_align(brow_r, LV_ALIGN_CENTER, 38, -54);
    lv_obj_set_style_transform_rotation(brow_l, 200, 0);   /* 20 deg */
    lv_obj_set_style_transform_rotation(brow_r, -200, 0);

    /* mouths */
    mouth_smile = lv_arc_create(face_cont);
    lv_obj_set_size(mouth_smile, 56, 56);
    lv_arc_set_bg_angles(mouth_smile, 40, 140);
    lv_obj_set_style_arc_width(mouth_smile, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(mouth_smile, C_EYE, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(mouth_smile, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(mouth_smile, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_style(mouth_smile, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(mouth_smile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(mouth_smile, LV_ALIGN_CENTER, 0, 32);

    mouth_frown = lv_arc_create(face_cont);
    lv_obj_set_size(mouth_frown, 56, 56);
    lv_arc_set_bg_angles(mouth_frown, 220, 320);
    lv_obj_set_style_arc_width(mouth_frown, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(mouth_frown, C_EYE, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(mouth_frown, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(mouth_frown, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_style(mouth_frown, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(mouth_frown, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(mouth_frown, LV_ALIGN_CENTER, 0, 72);

    mouth_o = make_box(face_cont);
    lv_obj_set_size(mouth_o, 20, 20);
    lv_obj_set_style_radius(mouth_o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(mouth_o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mouth_o, 5, 0);
    lv_obj_set_style_border_color(mouth_o, C_EYE, 0);
    lv_obj_align(mouth_o, LV_ALIGN_CENTER, 0, 44);

    mouth_flat = make_box(face_cont);
    lv_obj_set_size(mouth_flat, 34, 6);
    lv_obj_set_style_bg_color(mouth_flat, C_EYE, 0);
    lv_obj_set_style_bg_opa(mouth_flat, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mouth_flat, 3, 0);
    lv_obj_align(mouth_flat, LV_ALIGN_CENTER, 0, 54);

    /* zzz */
    zzz_lbl = make_label(face_cont, &lv_font_montserrat_20, C_SLEEP, "z Z z");
    lv_obj_align(zzz_lbl, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_anim_t za;
    lv_anim_init(&za);
    lv_anim_set_var(&za, zzz_lbl);
    lv_anim_set_exec_cb(&za, anim_opa_cb);
    lv_anim_set_values(&za, LV_OPA_20, LV_OPA_COVER);
    lv_anim_set_duration(&za, 1200);
    lv_anim_set_playback_duration(&za, 1200);
    lv_anim_set_repeat_count(&za, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&za);
}

/* Codex's face: a smaller, reduced-mood twin of build_face() above — same
 * panel/eye/mouth construction, but permanently blue-bordered (identity, not
 * mood) and limited to smile/flat mouths (no brows/frown/zzz). Hidden until
 * Codex reports any activity. */
static void build_codex_face(lv_obj_t *parent)
{
    cdx_face_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cdx_face_cont);
    lv_obj_set_size(cdx_face_cont, FACE_W - 14, SCR_H - 14);
    lv_obj_set_pos(cdx_face_cont, 7, 7);
    lv_obj_set_style_bg_color(cdx_face_cont, C_PANEL, 0);
    lv_obj_set_style_bg_opa(cdx_face_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cdx_face_cont, 16, 0);
    lv_obj_set_style_border_width(cdx_face_cont, 2, 0);
    lv_obj_set_style_border_color(cdx_face_cont, C_CODEX, 0);
    lv_obj_clear_flag(cdx_face_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cdx_face_cont, LV_OBJ_FLAG_HIDDEN);

    cdx_eye_l = make_box(cdx_face_cont);
    cdx_eye_r = make_box(cdx_face_cont);
    for (lv_obj_t *e : { cdx_eye_l, cdx_eye_r }) {
        lv_obj_set_style_bg_color(e, C_EYE, 0);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(e, LV_RADIUS_CIRCLE, 0);
    }
    lv_obj_set_size(cdx_eye_l, 36, 56);
    lv_obj_set_size(cdx_eye_r, 36, 56);
    lv_obj_align(cdx_eye_l, LV_ALIGN_CENTER, -38, -16);
    lv_obj_align(cdx_eye_r, LV_ALIGN_CENTER, 38, -16);

    cdx_pupil_l = make_box(cdx_eye_l);
    cdx_pupil_r = make_box(cdx_eye_r);
    for (lv_obj_t *p : { cdx_pupil_l, cdx_pupil_r }) {
        lv_obj_set_size(p, 14, 14);
        lv_obj_set_style_bg_color(p, C_PUPIL, 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(p, LV_ALIGN_CENTER, 0, 0);
    }

    cdx_mouth_smile = lv_arc_create(cdx_face_cont);
    lv_obj_set_size(cdx_mouth_smile, 56, 56);
    lv_arc_set_bg_angles(cdx_mouth_smile, 40, 140);
    lv_obj_set_style_arc_width(cdx_mouth_smile, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(cdx_mouth_smile, C_EYE, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(cdx_mouth_smile, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(cdx_mouth_smile, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_style(cdx_mouth_smile, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(cdx_mouth_smile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(cdx_mouth_smile, LV_ALIGN_CENTER, 0, 32);

    cdx_mouth_flat = make_box(cdx_face_cont);
    lv_obj_set_size(cdx_mouth_flat, 34, 6);
    lv_obj_set_style_bg_color(cdx_mouth_flat, C_EYE, 0);
    lv_obj_set_style_bg_opa(cdx_mouth_flat, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cdx_mouth_flat, 3, 0);
    lv_obj_align(cdx_mouth_flat, LV_ALIGN_CENTER, 0, 54);
}

static void apply_codex_mood(buddy_mood_t m)
{
    cur_mood_codex = m;
    switch (m) {
        case MOOD_WORKING:
            lv_obj_set_size(cdx_eye_l, 36, 44);
            lv_obj_set_size(cdx_eye_r, 36, 44);
            lv_obj_clear_flag(cdx_pupil_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cdx_pupil_r, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(cdx_pupil_l, LV_ALIGN_CENTER, 6, -5);
            lv_obj_align(cdx_pupil_r, LV_ALIGN_CENTER, 6, -5);
            lv_obj_clear_flag(cdx_mouth_flat, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(cdx_mouth_smile, LV_OBJ_FLAG_HIDDEN);
            break;
        case MOOD_HAPPY:
            /* short, squinty eyes — same trick Claude's face uses so a
               tall eye doesn't overlap the smile below it */
            lv_obj_set_size(cdx_eye_l, 40, 16);
            lv_obj_set_size(cdx_eye_r, 40, 16);
            lv_obj_add_flag(cdx_pupil_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(cdx_pupil_r, LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_bg_angles(cdx_mouth_smile, 20, 160);
            lv_obj_add_flag(cdx_mouth_flat, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cdx_mouth_smile, LV_OBJ_FLAG_HIDDEN);
            break;
        case MOOD_IDLE:
        default:
            lv_obj_set_size(cdx_eye_l, 36, 56);
            lv_obj_set_size(cdx_eye_r, 36, 56);
            lv_obj_clear_flag(cdx_pupil_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cdx_pupil_r, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(cdx_pupil_l, LV_ALIGN_CENTER, 0, 0);
            lv_obj_align(cdx_pupil_r, LV_ALIGN_CENTER, 0, 0);
            lv_arc_set_bg_angles(cdx_mouth_smile, 40, 140);
            lv_obj_add_flag(cdx_mouth_flat, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cdx_mouth_smile, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

static void build_buddy_screen(void)
{
    scr_buddy = make_screen();
    build_face(scr_buddy);
    build_codex_face(scr_buddy);

    /* Codex status text — mirrors headline_lbl/msg_lbl below, shown only
       when Codex has activity (see update_dual_layout()) */
    cdx_headline_lbl = make_label(scr_buddy, &lv_font_montserrat_20, C_CODEX, "");
    lv_obj_add_flag(cdx_headline_lbl, LV_OBJ_FLAG_HIDDEN);
    cdx_msg_lbl = make_label(scr_buddy, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_set_width(cdx_msg_lbl, 235);
    lv_label_set_long_mode(cdx_msg_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_add_flag(cdx_msg_lbl, LV_OBJ_FLAG_HIDDEN);

    /* middle column: what Claude is up to */
    headline_lbl = make_label(scr_buddy, &lv_font_montserrat_20, C_SLEEP, "waiting for companion");
    lv_obj_set_pos(headline_lbl, FACE_W + 18, 14);

    msg_lbl = make_label(scr_buddy, &lv_font_montserrat_14, C_TEXT, "run buddy_companion.py on your PC");
    lv_obj_set_width(msg_lbl, 235);
    lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(msg_lbl, FACE_W + 18, 46);

    proj_lbl = make_label(scr_buddy, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_set_pos(proj_lbl, FACE_W + 18, 70);

    /* now-playing line (shown while music plays) */
    med_line_lbl = make_label(scr_buddy, &lv_font_montserrat_12, C_WORKING, "");
    lv_obj_set_width(med_line_lbl, 235);
    lv_label_set_long_mode(med_line_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(med_line_lbl, FACE_W + 18, 126);
    lv_obj_add_flag(med_line_lbl, LV_OBJ_FLAG_HIDDEN);

    /* working dots */
    for (int i = 0; i < 3; i++) {
        dots[i] = make_box(scr_buddy);
        lv_obj_set_size(dots[i], 10, 10);
        lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dots[i], C_WORKING, 0);
        lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(dots[i], FACE_W + 18 + i * 18, 96);
        lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, dots[i]);
        lv_anim_set_exec_cb(&a, anim_opa_cb);
        lv_anim_set_values(&a, LV_OPA_20, LV_OPA_COVER);
        lv_anim_set_duration(&a, 350);
        lv_anim_set_playback_duration(&a, 350);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, i * 200);
        lv_anim_start(&a);
    }

    /* right column: clock, with date + battery tucked beside it (not below)
       to leave room for both Claude's and Codex's context/usage gauges */
    clk_lbl = make_label(scr_buddy, &lv_font_montserrat_28, C_EYE, "--:--");
    lv_obj_align(clk_lbl, LV_ALIGN_TOP_RIGHT, -16, 8);

    date_lbl = make_label(scr_buddy, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_set_pos(date_lbl, SCR_W - 166, 8);

    batt_lbl = make_label(scr_buddy, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_set_pos(batt_lbl, SCR_W - 166, 24);

    /* right column: Claude context/usage, then Codex context/usage below */
    ctx_lbl = make_label(scr_buddy, &lv_font_montserrat_12, C_TEXT, "claude ctx  --%");
    lv_obj_set_pos(ctx_lbl, SCR_W - 166, 44);
    ctx_bar = lv_bar_create(scr_buddy);
    lv_obj_clear_flag(ctx_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ctx_bar, 150, 8);
    lv_obj_set_pos(ctx_bar, SCR_W - 166, 60);
    lv_bar_set_range(ctx_bar, 0, 100);
    lv_obj_set_style_bg_color(ctx_bar, C_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ctx_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx_bar, C_HAPPY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ctx_bar, 4, LV_PART_INDICATOR);

    use_mini_lbl = make_label(scr_buddy, &lv_font_montserrat_12, C_DIM, "claude use --");
    lv_obj_set_pos(use_mini_lbl, SCR_W - 166, 72);
    use_mini_bar = lv_bar_create(scr_buddy);
    lv_obj_clear_flag(use_mini_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(use_mini_bar, 150, 8);
    lv_obj_set_pos(use_mini_bar, SCR_W - 166, 88);
    lv_bar_set_range(use_mini_bar, 0, 100);
    lv_obj_set_style_bg_color(use_mini_bar, C_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(use_mini_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(use_mini_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(use_mini_bar, C_HAPPY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(use_mini_bar, 4, LV_PART_INDICATOR);

    cdx_ctx_lbl = make_label(scr_buddy, &lv_font_montserrat_12, C_TEXT, "codex ctx  --%");
    lv_obj_set_pos(cdx_ctx_lbl, SCR_W - 166, 100);
    cdx_ctx_bar = lv_bar_create(scr_buddy);
    lv_obj_clear_flag(cdx_ctx_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(cdx_ctx_bar, 150, 8);
    lv_obj_set_pos(cdx_ctx_bar, SCR_W - 166, 116);
    lv_bar_set_range(cdx_ctx_bar, 0, 100);
    lv_obj_set_style_bg_color(cdx_ctx_bar, C_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cdx_ctx_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(cdx_ctx_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cdx_ctx_bar, C_HAPPY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(cdx_ctx_bar, 4, LV_PART_INDICATOR);

    cdx_use_lbl = make_label(scr_buddy, &lv_font_montserrat_12, C_DIM, "codex use --");
    lv_obj_set_pos(cdx_use_lbl, SCR_W - 166, 128);
    cdx_use_bar = lv_bar_create(scr_buddy);
    lv_obj_clear_flag(cdx_use_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(cdx_use_bar, 150, 8);
    lv_obj_set_pos(cdx_use_bar, SCR_W - 166, 144);
    lv_bar_set_range(cdx_use_bar, 0, 100);
    lv_obj_set_style_bg_color(cdx_use_bar, C_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cdx_use_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(cdx_use_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cdx_use_bar, C_HAPPY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(cdx_use_bar, 4, LV_PART_INDICATOR);

    /* album art peek: shown over the face on new tracks / swipe down */
    art_img_buddy = lv_image_create(scr_buddy);
    lv_obj_set_pos(art_img_buddy, 40, 26);
    lv_obj_add_flag(art_img_buddy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(art_img_buddy, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(art_img_buddy, 12, 0);
    lv_obj_set_style_clip_corner(art_img_buddy, true, 0);

    lv_obj_add_event_cb(scr_buddy, buddy_gesture_cb, LV_EVENT_GESTURE, NULL);
}

static void build_stats_screen(void)
{
    scr_stats = make_screen();

    stats_title_lbl = make_label(scr_stats, &lv_font_montserrat_12, C_DIM, "pc stats");
    lv_obj_set_pos(stats_title_lbl, 24, 10);

    cpu_lbl = make_label(scr_stats, &lv_font_montserrat_16, C_TEXT, "CPU  --%");
    lv_obj_set_pos(cpu_lbl, 40, 42);
    cpu_bar = make_hbar(scr_stats, 0);
    lv_obj_set_size(cpu_bar, 400, 14);
    lv_obj_align(cpu_bar, LV_ALIGN_TOP_RIGHT, -40, 44);
    lv_obj_set_style_bg_color(cpu_bar, C_WORKING, LV_PART_INDICATOR);

    ram_lbl = make_label(scr_stats, &lv_font_montserrat_16, C_TEXT, "RAM  --%");
    lv_obj_set_pos(ram_lbl, 40, 100);
    ram_bar = make_hbar(scr_stats, 0);
    lv_obj_set_size(ram_bar, 400, 14);
    lv_obj_align(ram_bar, LV_ALIGN_TOP_RIGHT, -40, 102);
    lv_obj_set_style_bg_color(ram_bar, C_IDLE, LV_PART_INDICATOR);

    stats_batt_lbl = make_label(scr_stats, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_align(stats_batt_lbl, LV_ALIGN_TOP_RIGHT, -24, 8);
}

static void build_zen_screen(void)
{
    scr_zen = make_screen();

    zen_clk_lbl = make_label(scr_zen, &lv_font_montserrat_48, C_EYE, "--:--");
    lv_obj_align(zen_clk_lbl, LV_ALIGN_CENTER, 0, -18);

    zen_date_lbl = make_label(scr_zen, &lv_font_montserrat_16, C_DIM, "");
    lv_obj_align(zen_date_lbl, LV_ALIGN_CENTER, 0, 26);

    zen_foot_lbl = make_label(scr_zen, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_align(zen_foot_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    zen_batt_lbl = make_label(scr_zen, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_align(zen_batt_lbl, LV_ALIGN_TOP_RIGHT, -12, 10);
}

static lv_obj_t *make_hbar(lv_obj_t *parent, int y)
{
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);  /* let taps pass through to the screen's page-cycle handler */
    lv_obj_set_size(b, 380, 12);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -30, y);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_bg_color(b, C_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, C_HAPPY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 6, LV_PART_INDICATOR);
    return b;
}

static void build_usage_screen(void)
{
    scr_use = make_screen();

    use_title_lbl = make_label(scr_use, &lv_font_montserrat_12, C_DIM, "claude usage");
    lv_obj_set_pos(use_title_lbl, 24, 10);

    use_pct_lbl = make_label(scr_use, &lv_font_montserrat_48, C_EYE, "--%");
    lv_obj_set_pos(use_pct_lbl, 24, 34);

    use_detail_lbl = make_label(scr_use, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_set_pos(use_detail_lbl, 24, 96);
    lv_obj_set_width(use_detail_lbl, 165);
    lv_label_set_long_mode(use_detail_lbl, LV_LABEL_LONG_WRAP);

    use_reset_lbl = make_label(scr_use, &lv_font_montserrat_12, C_TEXT, "5-hour");
    lv_obj_align(use_reset_lbl, LV_ALIGN_TOP_RIGHT, -30, 26);
    use_bar = make_hbar(scr_use, 44);

    week_all_lbl = make_label(scr_use, &lv_font_montserrat_12, C_TEXT, "weekly - all models");
    lv_obj_align(week_all_lbl, LV_ALIGN_TOP_RIGHT, -30, 70);
    week_all_bar = make_hbar(scr_use, 88);

    week_mdl_lbl = make_label(scr_use, &lv_font_montserrat_12, C_TEXT, "weekly - this model");
    lv_obj_align(week_mdl_lbl, LV_ALIGN_TOP_RIGHT, -30, 114);
    week_mdl_bar = make_hbar(scr_use, 132);

    use_batt_lbl = make_label(scr_use, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_align(use_batt_lbl, LV_ALIGN_TOP_RIGHT, -30, 8);
}

static void build_codex_usage_screen(void)
{
    scr_cdxu = make_screen();

    cdxu_title_lbl = make_label(scr_cdxu, &lv_font_montserrat_12, C_DIM, "codex usage");
    lv_obj_set_pos(cdxu_title_lbl, 24, 10);

    cdxu_pct_lbl = make_label(scr_cdxu, &lv_font_montserrat_48, C_CODEX, "--%");
    lv_obj_set_pos(cdxu_pct_lbl, 24, 34);

    cdxu_detail_lbl = make_label(scr_cdxu, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_set_pos(cdxu_detail_lbl, 24, 96);
    lv_obj_set_width(cdxu_detail_lbl, 165);
    lv_label_set_long_mode(cdxu_detail_lbl, LV_LABEL_LONG_WRAP);

    cdxu_reset_lbl = make_label(scr_cdxu, &lv_font_montserrat_12, C_TEXT, "usage");
    lv_obj_align(cdxu_reset_lbl, LV_ALIGN_TOP_RIGHT, -30, 26);
    cdxu_bar = make_hbar(scr_cdxu, 44);

    cdxu_batt_lbl = make_label(scr_cdxu, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_align(cdxu_batt_lbl, LV_ALIGN_TOP_RIGHT, -30, 8);
}

static lv_color_t usage_color(int pct)
{
    if (pct >= 85) return C_ERROR;
    if (pct >= 60) return C_WAITING;
    return C_HAPPY;
}

void buddy_set_usage(int pct, const char *reset, const char *block, const char *day)
{
    char buf[64];
    if (pct >= 0) {
        int shown = pct > 100 ? 100 : pct;
        lv_color_t c = usage_color(pct);
        lv_bar_set_value(use_mini_bar, shown, LV_ANIM_ON);
        lv_bar_set_value(use_bar, shown, LV_ANIM_ON);
        lv_obj_set_style_bg_color(use_mini_bar, c, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(use_bar, c, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(use_pct_lbl, buf);
        lv_obj_set_style_text_color(use_pct_lbl, c, 0);
        if (reset && reset[0])
            snprintf(buf, sizeof(buf), "claude use %d%% - %s", pct, reset);
        else
            snprintf(buf, sizeof(buf), "claude use %d%%", pct);
        lv_label_set_text(use_mini_lbl, buf);
    }
    if (reset && reset[0]) {
        snprintf(buf, sizeof(buf), "5-hour  -  resets %s", reset);
        lv_label_set_text(use_reset_lbl, buf);
    } else if (reset) {
        lv_label_set_text(use_reset_lbl, "5-hour  -  no active block");
    }
    if (block && day) {
        snprintf(buf, sizeof(buf), "block %s\ntoday %s", block, day);
        lv_label_set_text(use_detail_lbl, buf);
    }
}

void buddy_set_weekly(int all_pct, int model_pct, const char *reset)
{
    char buf[64];
    const char *rst = (reset && reset[0]) ? reset : "";
    if (all_pct >= 0) {
        lv_bar_set_value(week_all_bar, all_pct > 100 ? 100 : all_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(week_all_bar, usage_color(all_pct), LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "weekly all models  %d%%%s%s", all_pct,
                 rst[0] ? "  -  resets " : "", rst);
        lv_label_set_text(week_all_lbl, buf);
    }
    if (model_pct >= 0) {
        lv_bar_set_value(week_mdl_bar, model_pct > 100 ? 100 : model_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(week_mdl_bar, usage_color(model_pct), LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "weekly this model  %d%%", model_pct);
        lv_label_set_text(week_mdl_lbl, buf);
    }
}

void buddy_set_context(int pct, const char *txt)
{
    (void)txt;   /* full breakdown lives on the usage screen */
    char buf[40];
    if (pct >= 0) {
        int shown = pct > 100 ? 100 : pct;
        lv_bar_set_value(ctx_bar, shown, LV_ANIM_ON);
        lv_obj_set_style_bg_color(ctx_bar, usage_color(pct), LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "claude ctx  %d%%", pct);
        lv_label_set_text(ctx_lbl, buf);
    }
}

void buddy_set_codex(int ctx_pct, const char *ctx_txt, int use_pct, const char *reset)
{
    char buf[64];
    if (ctx_pct >= 0) {
        int shown = ctx_pct > 100 ? 100 : ctx_pct;
        lv_bar_set_value(cdx_ctx_bar, shown, LV_ANIM_ON);
        lv_obj_set_style_bg_color(cdx_ctx_bar, usage_color(ctx_pct), LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "codex ctx  %d%%", ctx_pct);
        lv_label_set_text(cdx_ctx_lbl, buf);
        if (ctx_txt && ctx_txt[0]) {
            snprintf(buf, sizeof(buf), "context\n%s", ctx_txt);
            lv_label_set_text(cdxu_detail_lbl, buf);
        }
    }
    if (use_pct >= 0) {
        int shown = use_pct > 100 ? 100 : use_pct;
        lv_color_t c = usage_color(use_pct);
        lv_bar_set_value(cdx_use_bar, shown, LV_ANIM_ON);
        lv_obj_set_style_bg_color(cdx_use_bar, c, LV_PART_INDICATOR);
        lv_bar_set_value(cdxu_bar, shown, LV_ANIM_ON);
        lv_obj_set_style_bg_color(cdxu_bar, c, LV_PART_INDICATOR);
        if (reset && reset[0])
            snprintf(buf, sizeof(buf), "codex use %d%% - %s", use_pct, reset);
        else
            snprintf(buf, sizeof(buf), "codex use %d%%", use_pct);
        lv_label_set_text(cdx_use_lbl, buf);
        snprintf(buf, sizeof(buf), "%d%%", use_pct);
        lv_label_set_text(cdxu_pct_lbl, buf);
        lv_obj_set_style_text_color(cdxu_pct_lbl, c, 0);
        if (reset && reset[0])
            snprintf(buf, sizeof(buf), "usage  -  resets in %s", reset);
        else
            snprintf(buf, sizeof(buf), "usage  -  no data yet");
        lv_label_set_text(cdxu_reset_lbl, buf);
    }
}

void buddy_set_codex_status(buddy_mood_t mood, const char *headline, const char *msg, const char *proj)
{
    (void)proj;   /* no room for a project line next to the reduced Codex face */
    if (mood != cur_mood_codex) apply_codex_mood(mood);
    if (headline) lv_label_set_text(cdx_headline_lbl, headline);
    if (msg) lv_label_set_text(cdx_msg_lbl, msg);
    update_dual_layout();
}

void buddy_set_battery(int pct, bool charging, bool ble)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const char *icon = pct > 87 ? LV_SYMBOL_BATTERY_FULL
                     : pct > 62 ? LV_SYMBOL_BATTERY_3
                     : pct > 37 ? LV_SYMBOL_BATTERY_2
                     : pct > 12 ? LV_SYMBOL_BATTERY_1
                                : LV_SYMBOL_BATTERY_EMPTY;
    char buf[48];
    snprintf(buf, sizeof(buf), "%s%s %d%%  %s",
             charging ? LV_SYMBOL_CHARGE " " : "", icon, pct,
             ble ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_USB);
    lv_color_t c = (!charging && pct <= 15) ? C_ERROR
                 : (!charging && pct <= 35) ? C_WAITING : C_DIM;
    lv_obj_t *lbls[6] = { batt_lbl, zen_batt_lbl, use_batt_lbl, cdxu_batt_lbl, stats_batt_lbl, med_batt_lbl };
    for (int i = 0; i < 6; i++) {
        lv_label_set_text(lbls[i], buf);
        lv_obj_set_style_text_color(lbls[i], c, 0);
    }
}

/* ---------- music motion: head bob + dancers + art peek ---------- */

static void anim_tx_cb(void *obj, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t *)obj, v, 0); }
static void anim_ty_cb(void *obj, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0); }
static void anim_rot_cb(void *obj, int32_t v) { lv_obj_set_style_transform_rotation((lv_obj_t *)obj, v, 0); }

/* gentle head bob on the main screen while music plays and buddy is idle;
   direction varies from bob to bob */
static void bob_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (med_playing != 1 || (cur_mood != MOOD_IDLE && cur_mood != MOOD_HAPPY)) {
        lv_obj_set_style_translate_x(face_cont, 0, 0);
        lv_obj_set_style_translate_y(face_cont, 0, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, face_cont);
    lv_anim_set_exec_cb(&a, lv_rand(0, 1) ? anim_tx_cb : anim_ty_cb);
    lv_anim_set_values(&a, -5, 5);
    lv_anim_set_duration(&a, 420);
    lv_anim_set_playback_duration(&a, 420);
    lv_anim_set_repeat_count(&a, 3);
    lv_anim_set_completed_cb(&a, [](lv_anim_t *an) {
        lv_obj_set_style_translate_x((lv_obj_t *)an->var, 0, 0);
        lv_obj_set_style_translate_y((lv_obj_t *)an->var, 0, 0);
    });
    lv_anim_start(&a);
}

/* the solo dancer: a little face in headphones that sways to the music */
static void build_dancer(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *d = make_box(parent);
    dancer = d;
    lv_obj_set_size(d, 84, 70);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_style_bg_color(d, C_PANEL, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d, 16, 0);
    lv_obj_set_style_border_width(d, 1, 0);
    lv_obj_set_style_border_color(d, C_BAR_BG, 0);
    lv_obj_set_style_transform_pivot_x(d, 42, 0);
    lv_obj_set_style_transform_pivot_y(d, 35, 0);

    /* headphones: band over the top + a pad on each side */
    lv_obj_t *band = lv_arc_create(d);
    lv_obj_set_size(band, 62, 62);
    lv_arc_set_bg_angles(band, 200, 340);
    lv_obj_set_style_arc_width(band, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(band, C_WORKING, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(band, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(band, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_style(band, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(band, LV_ALIGN_TOP_MID, 0, 2);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *pad = make_box(d);
        lv_obj_set_size(pad, 10, 20);
        lv_obj_set_style_bg_color(pad, C_WORKING, 0);
        lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pad, 4, 0);
        lv_obj_align(pad, i ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID,
                     i ? -4 : 4, -2);
    }

    dc_eye_l = make_box(d);
    dc_eye_r = make_box(d);
    lv_obj_t *eyes[2] = { dc_eye_l, dc_eye_r };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_size(eyes[i], 9, 16);
        lv_obj_set_style_bg_color(eyes[i], C_EYE, 0);
        lv_obj_set_style_bg_opa(eyes[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eyes[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_align(eyes[i], LV_ALIGN_CENTER, i ? 13 : -13, -6);
    }

    lv_obj_t *smile = lv_arc_create(d);
    lv_obj_set_size(smile, 26, 26);
    lv_arc_set_bg_angles(smile, 30, 150);
    lv_obj_set_style_arc_width(smile, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(smile, C_EYE, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(smile, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_style(smile, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(smile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(smile, LV_ALIGN_CENTER, 0, 15);

    /* easy sway, side to side */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d);
    lv_anim_set_exec_cb(&a, anim_tx_cb);
    lv_anim_set_values(&a, -10, 10);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_playback_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
}

static void dancer_blink_cb(lv_timer_t *t)
{
    if (lv_obj_has_flag(dancer, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_t *eyes[2] = { dc_eye_l, dc_eye_r };
    for (int i = 0; i < 2; i++) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, eyes[i]);
        lv_anim_set_exec_cb(&a, anim_height_cb);
        lv_anim_set_values(&a, 16, 4);
        lv_anim_set_duration(&a, 80);
        lv_anim_set_playback_duration(&a, 90);
        lv_anim_start(&a);
    }
    lv_timer_set_period(t, 2600 + lv_rand(0, 3200));
}

static void dancer_twirl_cb(lv_timer_t *t)
{
    if (lv_obj_has_flag(dancer, LV_OBJ_FLAG_HIDDEN)) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dancer);
    lv_anim_set_exec_cb(&a, anim_rot_cb);
    lv_anim_set_values(&a, 0, 3600);     /* one full spin */
    lv_anim_set_duration(&a, 700);
    lv_anim_set_completed_cb(&a, [](lv_anim_t *an) {
        lv_obj_set_style_transform_rotation((lv_obj_t *)an->var, 0, 0);
    });
    lv_anim_start(&a);
    lv_timer_set_period(t, 9000 + lv_rand(0, 9000));
}

static void dancers_set(bool on)
{
    if (on) lv_obj_clear_flag(dancer, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(dancer, LV_OBJ_FLAG_HIDDEN);
}

/* show the album art over the face for a moment */
static void art_hide_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_add_flag(art_img_buddy, LV_OBJ_FLAG_HIDDEN);
}

static void art_peek(uint32_t ms)
{
    if (!art_valid) return;
    lv_obj_clear_flag(art_img_buddy, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(art_hide_cb, ms, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void buddy_gesture_cb(lv_event_t *e)
{
    (void)e;
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM)
        art_peek(2500);
}

void buddy_media_art_ready(const uint8_t *pixels, int w, int h)
{
    art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    art_dsc.header.w = w;
    art_dsc.header.h = h;
    art_dsc.header.stride = w * 2;
    art_dsc.data_size = (uint32_t)(w * h * 2);
    art_dsc.data = pixels;
    lv_image_cache_drop(&art_dsc);
    lv_image_set_src(art_img_buddy, &art_dsc);
    lv_image_set_src(art_img_media, &art_dsc);
    lv_obj_clear_flag(art_img_media, LV_OBJ_FLAG_HIDDEN);
    art_valid = true;
    if (art_new_track) {
        art_new_track = false;
        art_peek(2200);
    }
}

static void media_btn_cb(lv_event_t *e)
{
    const char *cmd = (const char *)lv_event_get_user_data(e);
    if (media_cmd_cb) media_cmd_cb(cmd);
}

static lv_obj_t *make_media_btn(const char *sym, const char *cmd, int x_ofs)
{
    lv_obj_t *btn = lv_button_create(scr_media);
    lv_obj_set_size(btn, 64, 52);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, x_ofs, -12);
    lv_obj_set_style_bg_color(btn, C_PANEL, 0);
    lv_obj_set_style_bg_color(btn, C_BAR_BG, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, C_BAR_BG, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, media_btn_cb, LV_EVENT_CLICKED, (void *)cmd);
    lv_obj_t *l = make_label(btn, &lv_font_montserrat_20, C_EYE, sym);
    lv_obj_center(l);
    return btn;
}

static void fmt_mmss(char *out, size_t n, int s)
{
    if (s < 0) s = 0;
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void media_refresh_progress(void)
{
    char a[16], b[16], buf[40];
    fmt_mmss(a, sizeof(a), med_pos);
    fmt_mmss(b, sizeof(b), med_dur);
    snprintf(buf, sizeof(buf), "%s / %s", a, b);
    lv_label_set_text(med_time_lbl, buf);
    lv_bar_set_value(med_bar, (med_dur > 0) ? (med_pos * 100 / med_dur) : 0, LV_ANIM_OFF);
}

/* advance the progress bar locally so it moves smoothly between updates
   (browsers report position only sporadically) */
static void media_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (med_playing == 1 && med_dur > 0 && med_pos < med_dur) {
        med_pos++;
        media_refresh_progress();
    }
}

static void build_media_screen(void)
{
    scr_media = make_screen();

    /* album art, left */
    art_img_media = lv_image_create(scr_media);
    lv_obj_set_pos(art_img_media, 14, 12);
    lv_obj_set_style_radius(art_img_media, 10, 0);
    lv_obj_set_style_clip_corner(art_img_media, true, 0);
    lv_obj_clear_flag(art_img_media, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(art_img_media, LV_OBJ_FLAG_HIDDEN);

    /* track info, middle */
    med_app_lbl = make_label(scr_media, &lv_font_montserrat_12, C_DIM, "now playing");
    lv_obj_set_pos(med_app_lbl, 148, 12);

    med_title_lbl = make_label(scr_media, &lv_font_montserrat_20, C_EYE, "nothing playing");
    lv_obj_set_width(med_title_lbl, 290);
    lv_label_set_long_mode(med_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(med_title_lbl, 148, 38);

    med_artist_lbl = make_label(scr_media, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_set_width(med_artist_lbl, 290);
    lv_label_set_long_mode(med_artist_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(med_artist_lbl, 148, 70);

    med_time_lbl = make_label(scr_media, &lv_font_montserrat_12, C_DIM, "-:-- / -:--");
    lv_obj_set_pos(med_time_lbl, 148, 100);

    /* dance floor (behind the buttons; hidden unless playing) */
    build_dancer(scr_media, 508, 16);
    lv_timer_create(dancer_blink_cb, 3000, NULL);
    lv_timer_create(dancer_twirl_cb, 11000, NULL);

    /* transport buttons, on top of the dancers */
    med_btn_prev = make_media_btn(LV_SYMBOL_PREV, "prev", 0);
    med_btn_play = make_media_btn(LV_SYMBOL_PLAY, "play", 0);
    med_btn_next = make_media_btn(LV_SYMBOL_NEXT, "next", 0);
    lv_obj_align(med_btn_prev, LV_ALIGN_TOP_RIGHT, -170, 92);
    lv_obj_align(med_btn_play, LV_ALIGN_TOP_RIGHT, -98, 92);
    lv_obj_align(med_btn_next, LV_ALIGN_TOP_RIGHT, -26, 92);
    med_play_icon = lv_obj_get_child(med_btn_play, 0);

    /* progress bar along the very bottom */
    med_bar = make_hbar(scr_media, 0);
    lv_obj_set_size(med_bar, SCR_W - 48, 6);
    lv_obj_align(med_bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(med_bar, C_WORKING, LV_PART_INDICATOR);

    med_batt_lbl = make_label(scr_media, &lv_font_montserrat_12, C_DIM, "");
    lv_obj_align(med_batt_lbl, LV_ALIGN_TOP_RIGHT, -24, 8);

    lv_timer_create(media_tick_cb, 1000, NULL);
    lv_timer_create(bob_timer_cb, 3200, NULL);
}

void buddy_set_media_cmd_cb(void (*cb)(const char *cmd)) { media_cmd_cb = cb; }

void buddy_set_media(const char *title, const char *artist, const char *app,
                     int playing, int pos, int dur)
{
    static int med_last_reported = -1;
    bool same_track = (title != NULL &&
                       strncmp(med_last_title, title, sizeof(med_last_title) - 1) == 0);
    med_playing = playing;
    med_dur = dur;
    /* Web players freeze their reported position while playing and only
       refresh it on pause/seek. So: trust the local 1 s ticker, and accept
       the player's position only when it reports a NEW value (or the track
       changed) — a repeated value is stale, not a seek. */
    if (!same_track) {
        med_pos = pos;
    } else if (pos != med_last_reported) {
        if (pos > med_pos || (med_pos - pos) > 2) med_pos = pos;
    }
    med_last_reported = pos;

    if (playing < 0 || title == NULL || title[0] == '\0') {
        lv_label_set_text(med_title_lbl, "nothing playing");
        lv_label_set_text(med_artist_lbl, "");
        lv_label_set_text(med_app_lbl, "now playing");
        lv_label_set_text(med_time_lbl, "-:-- / -:--");
        lv_bar_set_value(med_bar, 0, LV_ANIM_OFF);
        lv_obj_add_flag(med_line_lbl, LV_OBJ_FLAG_HIDDEN);
        dancers_set(false);
        med_last_title[0] = '\0';
        return;
    }
    if (!same_track) {
        strncpy(med_last_title, title, sizeof(med_last_title) - 1);
        med_last_title[sizeof(med_last_title) - 1] = '\0';
        art_new_track = true;   /* peek the art when it finishes arriving */
    }
    lv_label_set_text(med_title_lbl, title);
    lv_label_set_text(med_artist_lbl, artist ? artist : "");
    char buf[192];
    snprintf(buf, sizeof(buf), "now playing  -  %s", app ? app : "");
    lv_label_set_text(med_app_lbl, buf);
    lv_label_set_text(med_play_icon, playing == 1 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    media_refresh_progress();

    /* buddy-screen ticker + dance floor follow the play state */
    if (playing == 1) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_AUDIO "  %s - %s", title,
                 artist ? artist : "");
        lv_label_set_text(med_line_lbl, buf);
        lv_obj_clear_flag(med_line_lbl, LV_OBJ_FLAG_HIDDEN);
        dancers_set(true);
    } else {
        lv_obj_add_flag(med_line_lbl, LV_OBJ_FLAG_HIDDEN);
        dancers_set(false);
    }
}

static void build_toast(void)
{
    toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(toast);
    lv_obj_set_size(toast, SCR_W - 40, 64);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(toast, C_PANEL, 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast, 12, 0);
    lv_obj_set_style_border_width(toast, 2, 0);
    lv_obj_set_style_border_color(toast, C_WAITING, 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

    toast_icon = make_label(toast, &lv_font_montserrat_20, C_WAITING, LV_SYMBOL_BELL);
    lv_obj_align(toast_icon, LV_ALIGN_LEFT_MID, 16, 0);

    toast_lbl = make_label(toast, &lv_font_montserrat_16, C_EYE, "");
    lv_obj_set_width(toast_lbl, SCR_W - 120);
    lv_label_set_long_mode(toast_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(toast_lbl, LV_ALIGN_LEFT_MID, 52, 0);
}

/* ---------- orientation: one widget set, two arrangements ---------- */

static void layout_apply(bool portrait)
{
    g_portrait = portrait;
    if (!portrait) {
        /* landscape 640x172 (the original layout) */
        lv_obj_set_size(face_cont, FACE_W - 14, 158);
        lv_obj_align(face_cont, LV_ALIGN_TOP_LEFT, 7, 7);
        lv_obj_align(headline_lbl, LV_ALIGN_TOP_LEFT, FACE_W + 18, 14);
        lv_obj_set_width(msg_lbl, 235);
        lv_obj_align(msg_lbl, LV_ALIGN_TOP_LEFT, FACE_W + 18, 46);
        lv_obj_align(proj_lbl, LV_ALIGN_TOP_LEFT, FACE_W + 18, 70);
        for (int i = 0; i < 3; i++)
            lv_obj_align(dots[i], LV_ALIGN_TOP_LEFT, FACE_W + 18 + i * 18, 96);
        lv_obj_set_width(med_line_lbl, 235);
        lv_obj_align(med_line_lbl, LV_ALIGN_TOP_LEFT, FACE_W + 18, 126);
        lv_obj_align(clk_lbl, LV_ALIGN_TOP_RIGHT, -16, 8);
        lv_obj_align(date_lbl, LV_ALIGN_TOP_LEFT, 640 - 166, 8);
        lv_obj_align(batt_lbl, LV_ALIGN_TOP_LEFT, 640 - 166, 24);
        lv_obj_align(ctx_lbl, LV_ALIGN_TOP_LEFT, 640 - 166, 44);
        lv_obj_set_size(ctx_bar, 150, 8);
        lv_obj_align(ctx_bar, LV_ALIGN_TOP_LEFT, 640 - 166, 60);
        lv_obj_align(use_mini_lbl, LV_ALIGN_TOP_LEFT, 640 - 166, 72);
        lv_obj_set_size(use_mini_bar, 150, 8);
        lv_obj_align(use_mini_bar, LV_ALIGN_TOP_LEFT, 640 - 166, 88);
        lv_obj_align(cdx_ctx_lbl, LV_ALIGN_TOP_LEFT, 640 - 166, 100);
        lv_obj_set_size(cdx_ctx_bar, 150, 8);
        lv_obj_align(cdx_ctx_bar, LV_ALIGN_TOP_LEFT, 640 - 166, 116);
        lv_obj_align(cdx_use_lbl, LV_ALIGN_TOP_LEFT, 640 - 166, 128);
        lv_obj_set_size(cdx_use_bar, 150, 8);
        lv_obj_align(cdx_use_bar, LV_ALIGN_TOP_LEFT, 640 - 166, 144);
        lv_obj_align(art_img_buddy, LV_ALIGN_TOP_LEFT, 40, 26);

        lv_obj_align(zen_clk_lbl, LV_ALIGN_CENTER, 0, -18);
        lv_obj_align(zen_date_lbl, LV_ALIGN_CENTER, 0, 26);
        lv_obj_align(zen_foot_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_align(zen_batt_lbl, LV_ALIGN_TOP_RIGHT, -12, 10);

        lv_obj_align(use_title_lbl, LV_ALIGN_TOP_LEFT, 24, 10);
        lv_obj_align(use_pct_lbl, LV_ALIGN_TOP_LEFT, 24, 34);
        lv_obj_set_width(use_detail_lbl, 165);
        lv_obj_align(use_detail_lbl, LV_ALIGN_TOP_LEFT, 24, 96);
        lv_obj_align(use_reset_lbl, LV_ALIGN_TOP_RIGHT, -30, 26);
        lv_obj_set_size(use_bar, 380, 12);
        lv_obj_align(use_bar, LV_ALIGN_TOP_RIGHT, -30, 44);
        lv_obj_align(week_all_lbl, LV_ALIGN_TOP_RIGHT, -30, 70);
        lv_obj_set_size(week_all_bar, 380, 12);
        lv_obj_align(week_all_bar, LV_ALIGN_TOP_RIGHT, -30, 88);
        lv_obj_align(week_mdl_lbl, LV_ALIGN_TOP_RIGHT, -30, 114);
        lv_obj_set_size(week_mdl_bar, 380, 12);
        lv_obj_align(week_mdl_bar, LV_ALIGN_TOP_RIGHT, -30, 132);
        lv_obj_align(use_batt_lbl, LV_ALIGN_TOP_RIGHT, -30, 8);

        lv_obj_align(cdxu_title_lbl, LV_ALIGN_TOP_LEFT, 24, 10);
        lv_obj_align(cdxu_pct_lbl, LV_ALIGN_TOP_LEFT, 24, 34);
        lv_obj_set_width(cdxu_detail_lbl, 165);
        lv_obj_align(cdxu_detail_lbl, LV_ALIGN_TOP_LEFT, 24, 96);
        lv_obj_align(cdxu_reset_lbl, LV_ALIGN_TOP_RIGHT, -30, 26);
        lv_obj_set_size(cdxu_bar, 380, 12);
        lv_obj_align(cdxu_bar, LV_ALIGN_TOP_RIGHT, -30, 44);
        lv_obj_align(cdxu_batt_lbl, LV_ALIGN_TOP_RIGHT, -30, 8);

        lv_obj_align(stats_title_lbl, LV_ALIGN_TOP_LEFT, 24, 10);
        lv_obj_align(cpu_lbl, LV_ALIGN_TOP_LEFT, 40, 42);
        lv_obj_set_size(cpu_bar, 400, 14);
        lv_obj_align(cpu_bar, LV_ALIGN_TOP_RIGHT, -40, 44);
        lv_obj_align(ram_lbl, LV_ALIGN_TOP_LEFT, 40, 100);
        lv_obj_set_size(ram_bar, 400, 14);
        lv_obj_align(ram_bar, LV_ALIGN_TOP_RIGHT, -40, 102);
        lv_obj_align(stats_batt_lbl, LV_ALIGN_TOP_RIGHT, -24, 8);

        lv_obj_align(art_img_media, LV_ALIGN_TOP_LEFT, 14, 12);
        lv_obj_align(med_app_lbl, LV_ALIGN_TOP_LEFT, 148, 12);
        lv_obj_set_width(med_title_lbl, 290);
        lv_obj_align(med_title_lbl, LV_ALIGN_TOP_LEFT, 148, 38);
        lv_obj_set_width(med_artist_lbl, 290);
        lv_obj_align(med_artist_lbl, LV_ALIGN_TOP_LEFT, 148, 70);
        lv_obj_align(med_time_lbl, LV_ALIGN_TOP_LEFT, 148, 100);
        lv_obj_align(dancer, LV_ALIGN_TOP_LEFT, 508, 16);
        lv_obj_align(med_btn_prev, LV_ALIGN_TOP_RIGHT, -170, 92);
        lv_obj_align(med_btn_play, LV_ALIGN_TOP_RIGHT, -98, 92);
        lv_obj_align(med_btn_next, LV_ALIGN_TOP_RIGHT, -26, 92);
        lv_obj_set_size(med_bar, 640 - 48, 6);
        lv_obj_align(med_bar, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_align(med_batt_lbl, LV_ALIGN_TOP_RIGHT, -24, 8);

        lv_obj_set_size(toast, 640 - 40, 64);
        lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_width(toast_lbl, 640 - 120);
    } else {
        /* portrait 172x640: everything stacks */
        lv_obj_set_size(face_cont, 160, 150);
        lv_obj_align(face_cont, LV_ALIGN_TOP_LEFT, 6, 6);
        lv_obj_align(headline_lbl, LV_ALIGN_TOP_LEFT, 10, 168);
        lv_obj_set_width(msg_lbl, 152);
        lv_obj_align(msg_lbl, LV_ALIGN_TOP_LEFT, 10, 198);
        lv_obj_align(proj_lbl, LV_ALIGN_TOP_LEFT, 10, 224);
        for (int i = 0; i < 3; i++)
            lv_obj_align(dots[i], LV_ALIGN_TOP_LEFT, 10 + i * 18, 250);
        lv_obj_set_width(med_line_lbl, 152);
        lv_obj_align(med_line_lbl, LV_ALIGN_TOP_LEFT, 10, 272);
        lv_obj_align(clk_lbl, LV_ALIGN_TOP_LEFT, 10, 306);
        lv_obj_align(date_lbl, LV_ALIGN_TOP_LEFT, 10, 342);
        lv_obj_align(batt_lbl, LV_ALIGN_TOP_LEFT, 10, 360);
        lv_obj_align(ctx_lbl, LV_ALIGN_TOP_LEFT, 10, 396);
        lv_obj_set_size(ctx_bar, 152, 8);
        lv_obj_align(ctx_bar, LV_ALIGN_TOP_LEFT, 10, 414);
        lv_obj_align(use_mini_lbl, LV_ALIGN_TOP_LEFT, 10, 436);
        lv_obj_set_size(use_mini_bar, 152, 8);
        lv_obj_align(use_mini_bar, LV_ALIGN_TOP_LEFT, 10, 454);
        lv_obj_align(cdx_ctx_lbl, LV_ALIGN_TOP_LEFT, 10, 476);
        lv_obj_set_size(cdx_ctx_bar, 152, 8);
        lv_obj_align(cdx_ctx_bar, LV_ALIGN_TOP_LEFT, 10, 494);
        lv_obj_align(cdx_use_lbl, LV_ALIGN_TOP_LEFT, 10, 516);
        lv_obj_set_size(cdx_use_bar, 152, 8);
        lv_obj_align(cdx_use_bar, LV_ALIGN_TOP_LEFT, 10, 534);
        lv_obj_align(art_img_buddy, LV_ALIGN_TOP_LEFT, 26, 20);

        lv_obj_align(zen_clk_lbl, LV_ALIGN_TOP_MID, 0, 150);
        lv_obj_align(zen_date_lbl, LV_ALIGN_TOP_MID, 0, 214);
        lv_obj_align(zen_foot_lbl, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_align(zen_batt_lbl, LV_ALIGN_TOP_RIGHT, -8, 8);

        lv_obj_align(use_title_lbl, LV_ALIGN_TOP_LEFT, 10, 8);
        lv_obj_align(use_pct_lbl, LV_ALIGN_TOP_LEFT, 10, 32);
        lv_obj_align(use_reset_lbl, LV_ALIGN_TOP_LEFT, 10, 96);
        lv_obj_set_size(use_bar, 152, 12);
        lv_obj_align(use_bar, LV_ALIGN_TOP_LEFT, 10, 116);
        lv_obj_align(week_all_lbl, LV_ALIGN_TOP_LEFT, 10, 154);
        lv_obj_set_size(week_all_bar, 152, 12);
        lv_obj_align(week_all_bar, LV_ALIGN_TOP_LEFT, 10, 174);
        lv_obj_align(week_mdl_lbl, LV_ALIGN_TOP_LEFT, 10, 212);
        lv_obj_set_size(week_mdl_bar, 152, 12);
        lv_obj_align(week_mdl_bar, LV_ALIGN_TOP_LEFT, 10, 232);
        lv_obj_set_width(use_detail_lbl, 152);
        lv_obj_align(use_detail_lbl, LV_ALIGN_TOP_LEFT, 10, 272);
        lv_obj_align(use_batt_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

        lv_obj_align(cdxu_title_lbl, LV_ALIGN_TOP_LEFT, 10, 8);
        lv_obj_align(cdxu_pct_lbl, LV_ALIGN_TOP_LEFT, 10, 32);
        lv_obj_align(cdxu_reset_lbl, LV_ALIGN_TOP_LEFT, 10, 96);
        lv_obj_set_size(cdxu_bar, 152, 12);
        lv_obj_align(cdxu_bar, LV_ALIGN_TOP_LEFT, 10, 116);
        lv_obj_set_width(cdxu_detail_lbl, 152);
        lv_obj_align(cdxu_detail_lbl, LV_ALIGN_TOP_LEFT, 10, 156);
        lv_obj_align(cdxu_batt_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

        lv_obj_align(stats_title_lbl, LV_ALIGN_TOP_LEFT, 10, 8);
        lv_obj_align(cpu_lbl, LV_ALIGN_TOP_LEFT, 10, 60);
        lv_obj_set_size(cpu_bar, 152, 14);
        lv_obj_align(cpu_bar, LV_ALIGN_TOP_LEFT, 10, 86);
        lv_obj_align(ram_lbl, LV_ALIGN_TOP_LEFT, 10, 130);
        lv_obj_set_size(ram_bar, 152, 14);
        lv_obj_align(ram_bar, LV_ALIGN_TOP_LEFT, 10, 156);
        lv_obj_align(stats_batt_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

        lv_obj_align(art_img_media, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_align(med_app_lbl, LV_ALIGN_TOP_LEFT, 10, 138);
        lv_obj_set_width(med_title_lbl, 152);
        lv_obj_align(med_title_lbl, LV_ALIGN_TOP_LEFT, 10, 162);
        lv_obj_set_width(med_artist_lbl, 152);
        lv_obj_align(med_artist_lbl, LV_ALIGN_TOP_LEFT, 10, 194);
        lv_obj_align(med_time_lbl, LV_ALIGN_TOP_LEFT, 10, 220);
        lv_obj_align(med_btn_prev, LV_ALIGN_TOP_LEFT, 8, 252);
        lv_obj_align(med_btn_play, LV_ALIGN_TOP_MID, 0, 252);
        lv_obj_align(med_btn_next, LV_ALIGN_TOP_RIGHT, -8, 252);
        lv_obj_align(dancer, LV_ALIGN_TOP_MID, 0, 330);
        lv_obj_set_size(med_bar, 148, 6);
        lv_obj_align(med_bar, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_align(med_batt_lbl, LV_ALIGN_BOTTOM_MID, 0, -28);

        lv_obj_set_size(toast, 148, 64);
        lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_width(toast_lbl, 88);
    }
    update_dual_layout();
}

void buddy_set_orientation(int mode)
{
    static int cur = 0;
    if (mode == cur) return;
    cur = mode;
    lv_display_t *disp = lv_display_get_default();
    lv_display_set_rotation(disp,
        mode == 0 ? LV_DISPLAY_ROTATION_270 :
        mode == 1 ? LV_DISPLAY_ROTATION_0 :
        mode == 2 ? LV_DISPLAY_ROTATION_180 : LV_DISPLAY_ROTATION_90);
    layout_apply(mode == 1 || mode == 2);
}

void buddy_ui_init(void)
{
    build_buddy_screen();
    build_zen_screen();
    build_usage_screen();
    build_codex_usage_screen();
    build_stats_screen();
    build_media_screen();
    build_toast();

    lv_timer_create(blink_timer_cb, 3000, NULL);
    lv_timer_create(wander_timer_cb, 900, NULL);

    apply_mood(MOOD_SLEEP);
    lv_screen_load(scr_buddy);
}
