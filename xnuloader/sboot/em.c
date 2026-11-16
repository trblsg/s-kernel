/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite EngineerMode: the boot menu's service tool, in the style of the
 * MediaTek EngineerMode of mid-2010s Android phones (Holo Dark: status bar,
 * action bar, scrolling tab strip with the blue indicator, plain list rows,
 * Back/Home/Recents navigation bar), for a Galaxy S9 running iOS, Windows and
 * a Linux kiosk from one boot menu.
 *
 * Keys: Volume moves, Power selects, Bixby goes back (on a top-level tab: next
 * tab), holding Bixby leaves EngineerMode. Touch: tap, drag to scroll, swipe
 * sideways to change tabs; the navigation bar's Back / Home / Recents work as
 * on Android (Recents = next tab).
 *
 * Everything here reads hardware; writes are limited to what the old
 * Starbite Engineer already did (PMIC touch rails behind a dialog, reboots,
 * the iOS boot-args) and are marked in their dialogs.
 * No pointer tables: the Image must have no absolute relocations, so lists
 * are filled at run time and names come from switch statements.
 */
#include <stdint.h>
#include "hw.h"
#include "ui.h"
#include "fbtext.h"
#include "fdt.h"
#include "mmu.h"
#include "ufs.h"
#include "store.h"
#include "catalog.h"
#include "menu.h"
#include "sd.h"

/* ---- geometry ----------------------------------------------------------- */

#define SB_H 72
#define BAR_Y 72
#define BAR_H 168
#define TAB_Y 240
#define TAB_H 144
#define NAV_Y 2800
#define NAV_H 160
#define TOP_Y0 (TAB_Y + TAB_H)
#define PAGE_Y0 (BAR_Y + BAR_H)
#define PAD 48
#define RIGHT (UI_W - PAD)

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static struct {
    const struct star_info *in;
    char *cmd;
    unsigned len;
    int boot;           /* 1: leave and boot iOS */
    int home;           /* leave EngineerMode */
    uintptr_t args;     /* S-Boot's bootargs in the FDT */
    uint32_t args_len;
} E;

/* ---- input -------------------------------------------------------------- */

enum { EV_NONE, EV_UP, EV_DOWN, EV_SELECT, EV_BACK, EV_HOME, EV_TAP, EV_DRAG, EV_SWIPE_L, EV_SWIPE_R };

struct ev {
    int type, x, y, d;
};

static unsigned held_keys;
static int t_down, t_x0, t_y0, t_ly, t_moved;

/* Next input event; EV_NONE after timeout_ms (0 = wait forever). */
static void ev_wait(struct ev *e, unsigned timeout_ms)
{
    uint64_t t0 = hw_ticks(), lim = timeout_ms ? hw_freq() * timeout_ms / 1000U : 0;
    e->type = EV_NONE;
    for (;;) {
        unsigned k = hw_keys();
        if (k && !held_keys) {
            hw_wait_ms(30);
            if (hw_keys() == k) {
                held_keys = k;
                if (k == K_BIXBY) {
                    for (unsigned t = 30; t < 700; t += 10) {
                        if (!(hw_keys() & K_BIXBY)) {
                            held_keys = 0;
                            e->type = EV_BACK;
                            return;
                        }
                        hw_wait_ms(10);
                    }
                    e->type = EV_HOME;
                    return;
                }
                e->type = (k & K_UP) ? EV_UP : (k & K_DOWN) ? EV_DOWN : (k & K_POWER) ? EV_SELECT : EV_NONE;
                if (e->type != EV_NONE)
                    return;
            }
        }
        if (!k)
            held_keys = 0;
        int st, x, y;
        while (hw_tp_event(&st, &x, &y)) {
            if (st == 1) {
                t_down = 1;
                t_x0 = x;
                t_y0 = y;
                t_ly = y;
                t_moved = 0;
            } else if (st == 2 && t_down) {
                if (!t_moved && iabs(y - t_y0) > 36)
                    t_moved = 1;
                else if (!t_moved && iabs(x - t_x0) > 60)
                    t_moved = 2;
                if (t_moved == 1 && y != t_ly) {
                    e->type = EV_DRAG;
                    e->d = y - t_ly;
                    e->x = x;
                    e->y = y;
                    t_ly = y;
                    return;
                }
            } else if (st == 3 && t_down) {
                int dx = x - t_x0, dy = y - t_y0;
                t_down = 0;
                if (!t_moved && iabs(dx) < 60 && iabs(dy) < 60) {
                    e->type = EV_TAP;
                    e->x = t_x0;
                    e->y = t_y0;
                    return;
                }
                if (t_moved != 1 && iabs(dx) > 280 && iabs(dy) < 260) {
                    e->type = dx < 0 ? EV_SWIPE_L : EV_SWIPE_R;
                    return;
                }
            }
        }
        if (lim && hw_ticks() - t0 >= lim)
            return;
        hw_wait_ms(4);
    }
}

/* ---- PMIC RTC (S2MPS18 RTC block = ACPM type 2) ------------------------- */

struct rtc {
    unsigned y, mo, d, h, mi, s, ctrl;
    int ok;
};

/* Latch with UPDATE RUDR (keep RTC_WAKE and unknown bits), then SEC..YEAR
 * 0x05..0x0B in binary 24 h (build/woa/rtc-research.md). */
static void rtc_read(struct rtc *r)
{
    uint8_t u = 0, ctrl = 0, v[7];
    r->ok = 0;
    if (!hw_pmic_read(2, 0x00, &ctrl) || !hw_pmic_read(2, 0x02, &u))
        return;
    if (!hw_pmic_write(2, 0x02, (uint8_t)((u & ~0x17U) | 0x01U)))
        return;
    hw_wait_ms(2);
    for (unsigned i = 0; i < 7; ++i)
        if (!hw_pmic_read(2, 0x05 + i, &v[i]))
            return;
    r->s = v[0] & 0x7fU;
    r->mi = v[1] & 0x7fU;
    r->h = v[2] & 0x1fU;
    r->d = v[4] & 0x1fU;
    r->mo = v[5] & 0x0fU;
    r->y = 2000U + (v[6] & 0x7fU);
    r->ctrl = ctrl;
    r->ok = r->s < 60 && r->mi < 60 && r->h < 24 && r->d >= 1 && r->d <= 31 && r->mo >= 1 && r->mo <= 12;
}

/* PMIC RTC as Unix time (UTC), for Linux (xnu_boot.c star_linux_store). */
int em_rtc_epoch(uint64_t *epoch)
{
    struct rtc r;
    rtc_read(&r);
    if (!r.ok)
        return 0;
    /* days from civil (Howard Hinnant), year >= 2000 */
    unsigned y = r.y - (r.mo <= 2), era = y / 400, yoe = y - era * 400;
    unsigned doy = (153 * (r.mo + (r.mo > 2 ? -3 : 9)) + 2) / 5 + r.d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint64_t days = (uint64_t)era * 146097 + doe - 719468;
    *epoch = days * 86400 + r.h * 3600 + r.mi * 60 + r.s;
    return 1;
}

/* ---- chrome ------------------------------------------------------------- */

static int cur_tab;

static void draw_status(void)
{
    struct rtc r;
    struct ub u;
    ui_clip(0, UI_H);
    ui_fill(0, 0, UI_W, SB_H, UI_BLACK);
    ui_text(F_STATUS, 36, 50, "STARBITE", UI_HOLO, UI_BLACK);
    ui_text(F_STATUS, 220, 50, "SM-G960F", UI_TEXT3, UI_BLACK);
    rtc_read(&r);
    ub_init(&u);
    if (r.ok) {
        ub_dec2(&u, r.h);
        ub_c(&u, ':');
        ub_dec2(&u, r.mi);
        ub_s(&u, " UTC");
    } else {
        ub_s(&u, "--:--");
    }
    ui_text_r(F_STATUS, UI_W - 36, 50, u.b, UI_TEXT, UI_BLACK);
}

static void draw_bar(const char *title, int up)
{
    ui_clip(0, UI_H);
    ui_fill(0, BAR_Y, UI_W, BAR_H, UI_BAR);
    if (up) {
        (void)star_sprite_at(201, 8, BAR_Y + 36);
        (void)star_sprite_at(200, 64, BAR_Y + 24);
        ui_text(F_TITLE, 210, BAR_Y + 106, title, UI_TEXT, UI_BAR);
    } else {
        (void)star_sprite_at(200, 28, BAR_Y + 24);
        ui_text(F_TITLE, 176, BAR_Y + 106, title, UI_TEXT, UI_BAR);
    }
}

static void draw_nav(void)
{
    ui_clip(0, UI_H);
    ui_fill(0, NAV_Y, UI_W, NAV_H, UI_BLACK);
    ui_fill(0, NAV_Y, UI_W, 2, UI_DIV);
    (void)star_sprite_at(202, UI_W / 6 - 52, NAV_Y + 28);
    (void)star_sprite_at(203, UI_W / 2 - 52, NAV_Y + 28);
    (void)star_sprite_at(204, UI_W * 5 / 6 - 52, NAV_Y + 28);
}

/* Navigation bar / action bar taps: 1 back, 2 home, 3 recents, 0 none. */
static int chrome_tap(const struct ev *e, int up)
{
    if (e->y >= NAV_Y)
        return e->x < UI_W / 3 ? 1 : e->x < UI_W * 2 / 3 ? 2 : 3;
    if (up && e->y >= BAR_Y && e->y < BAR_Y + BAR_H && e->x < 640)
        return 1;
    return 0;
}

/* ---- Holo dialog -------------------------------------------------------- */

/* Two-button AlertDialog; returns 1 for OK. Volume moves, Power picks,
 * Bixby cancels; tap a button. */
static int dialog(const char *title, const char *l1, const char *l2, const char *ok)
{
    const int x = 110, w = UI_W - 220, y = 980, h = 760, by = y + h - 160;
    int sel = 1;
    for (;;) {
        ui_clip(0, UI_H);
        ui_fill(x - 4, y - 4, w + 8, h + 8, 0xff3a3a3aU);
        ui_fill(x, y, w, h, UI_DIALOG);
        ui_text(F_TITLE, x + 60, y + 120, title, UI_HOLO, UI_DIALOG);
        ui_fill(x, y + 180, w, 4, UI_HOLO);
        ui_text(F_BODY, x + 60, y + 300, l1, UI_TEXT, UI_DIALOG);
        if (l2)
            ui_text(F_BODY, x + 60, y + 380, l2, UI_TEXT2, UI_DIALOG);
        ui_fill(x, by, w, 2, 0xff3a3a3aU);
        ui_fill(x + w / 2 - 1, by, 2, 160, 0xff3a3a3aU);
        ui_fill(x, by + 2, w / 2 - 1, 158, sel == 0 ? UI_FOCUS : UI_DIALOG);
        ui_fill(x + w / 2 + 1, by + 2, w / 2 - 1, 158, sel == 1 ? UI_FOCUS : UI_DIALOG);
        ui_text_c(F_TAB, x + w / 4, by + 94, "CANCEL", UI_TEXT, sel == 0 ? UI_FOCUS : UI_DIALOG);
        ui_text_c(F_TAB, x + 3 * w / 4, by + 94, ok, UI_TEXT, sel == 1 ? UI_FOCUS : UI_DIALOG);
        struct ev e;
        ev_wait(&e, 0);
        if (e.type == EV_UP || e.type == EV_DOWN)
            sel ^= 1;
        else if (e.type == EV_SELECT)
            return sel;
        else if (e.type == EV_BACK || e.type == EV_HOME)
            return 0;
        else if (e.type == EV_TAP && e.y >= by && e.y < by + 160 && e.x >= x && e.x < x + w)
            return e.x >= x + w / 2;
    }
}

/* ---- list screens -------------------------------------------------------- */

#define R_DIS 1U        /* greyed, not selectable */
#define R_CHECK 2U      /* checkbox at the right */
#define R_ON 4U         /* checkbox ticked */
#define R_WARN 8U       /* title in Holo orange */
#define R_RED 16U       /* title in Holo red */

#define MAX_ROWS 40

static struct row {
    char t[80];
    char s[120];
    uint32_t f;
} rows[MAX_ROWS];
static int nrows;

static void row_add(const char *t, const char *s, uint32_t f)
{
    if (nrows >= MAX_ROWS)
        return;
    struct row *r = &rows[nrows++];
    unsigned i = 0;
    for (; t[i] && i < sizeof(r->t) - 1; ++i)
        r->t[i] = t[i];
    r->t[i] = 0;
    i = 0;
    if (s)
        for (; s[i] && i < sizeof(r->s) - 1; ++i)
            r->s[i] = s[i];
    r->s[i] = 0;
    r->f = f;
}

static int row_h(const struct row *r)
{
    return r->s[0] ? 196 : 156;
}

static void draw_rows(int y0, int y1, int scroll, int focus)
{
    ui_clip(y0, y1);
    int y = y0 - scroll;
    for (int i = 0; i < nrows; ++i) {
        const struct row *r = &rows[i];
        int h = row_h(r);
        if (y + h > y0 && y < y1) {
            uint32_t bg = i == focus ? UI_FOCUS : UI_BG;
            uint32_t tc = (r->f & R_DIS) ? UI_TEXT3 : (r->f & R_RED) ? UI_RED : (r->f & R_WARN) ? UI_ORANGE : UI_TEXT;
            ui_fill(0, y, UI_W, h - 2, bg);
            if (r->s[0]) {
                ui_text(F_BODY, PAD, y + 84, r->t, tc, bg);
                ui_text(F_SMALL, PAD, y + 146, r->s, (r->f & R_DIS) ? UI_TEXT3 : UI_TEXT2, bg);
            } else {
                ui_text(F_BODY, PAD, y + h / 2 + 16, r->t, tc, bg);
            }
            if (r->f & R_CHECK) {
                int cx = UI_W - PAD - 64, cy = y + (h - 64) / 2;
                ui_fill(cx, cy, 64, 64, (r->f & R_ON) ? UI_HOLO : 0xff8a8a8aU);
                ui_fill(cx + 5, cy + 5, 54, 54, (r->f & R_ON) ? UI_HOLO : bg);
                if (r->f & R_ON)
                    ui_text_c(F_TAB, cx + 32, cy + 48, UI_CHECK, UI_BLACK, UI_HOLO);
            }
            ui_fill(0, y + h - 2, UI_W, 2, UI_DIV);
        }
        y += h;
    }
    if (y < y1)
        ui_fill(0, y, UI_W, y1 - y, UI_BG);
    ui_clip(0, UI_H);
}

static int rows_total(void)
{
    int t = 0;
    for (int i = 0; i < nrows; ++i)
        t += row_h(&rows[i]);
    return t;
}

static int row_top(int idx)
{
    int t = 0;
    for (int i = 0; i < idx && i < nrows; ++i)
        t += row_h(&rows[i]);
    return t;
}

static int row_at(int y0, int scroll, int y)
{
    int t = y0 - scroll;
    for (int i = 0; i < nrows; ++i) {
        int h = row_h(&rows[i]);
        if (y >= t && y < t + h)
            return i;
        t += h;
    }
    return -1;
}

static int clamp_scroll(int s, int total, int view)
{
    if (s > total - view)
        s = total - view;
    return s < 0 ? 0 : s;
}

static int keep_visible(int focus, int scroll, int view)
{
    int top = row_top(focus), h = focus >= 0 && focus < nrows ? row_h(&rows[focus]) : 0;
    if (top < scroll)
        return top;
    if (top + h > scroll + view)
        return top + h - view;
    return scroll;
}

static int next_enabled(int from, int dir)
{
    for (int i = from + dir; i >= 0 && i < nrows; i += dir)
        if (!(rows[i].f & R_DIS))
            return i;
    return from;
}

/* ---- key/value info screens ------------------------------------------------ */

#define KV_ROW 0
#define KV_HDR 1
#define KV_MONO 2
#define KV_BIG 3
#define MAX_KV 200

static struct kvr {
    char k[56];
    char v[150];
    uint32_t c;
    uint8_t kind;
} kv[MAX_KV];
static int nkv;

static void copy(char *d, unsigned cap, const char *s)
{
    unsigned i = 0;
    if (s)
        for (; s[i] && i < cap - 1; ++i)
            d[i] = s[i];
    d[i] = 0;
}

static void kv_put(int kind, const char *k, const char *v, uint32_t c)
{
    if (nkv >= MAX_KV)
        return;
    struct kvr *r = &kv[nkv++];
    r->kind = (uint8_t)kind;
    copy(r->k, sizeof(r->k), k);
    copy(r->v, sizeof(r->v), v);
    r->c = c;
}

static void kv_hdr(const char *t) { kv_put(KV_HDR, t, 0, UI_HOLO); }
static void kv_add(const char *k, const char *v) { kv_put(KV_ROW, k, v, UI_TEXT); }
static void kv_addc(const char *k, const char *v, uint32_t c) { kv_put(KV_ROW, k, v, c); }
static void kv_mono(const char *t) { kv_put(KV_MONO, 0, t, UI_TEXT); }
static void kv_big(const char *k, const char *v) { kv_put(KV_BIG, k, v, UI_TEXT); }

static void kv_hex(const char *k, uint64_t v, unsigned digits)
{
    struct ub u;
    ub_init(&u);
    ub_hex(&u, v, digits);
    kv_add(k, u.b);
}

static void kv_dec(const char *k, uint64_t v, const char *unit)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, v);
    if (unit)
        ub_s(&u, unit);
    kv_add(k, u.b);
}

static int kv_h(const struct kvr *r)
{
    switch (r->kind) {
    case KV_HDR:
        return 112;
    case KV_MONO:
        return 46;
    case KV_BIG:
        return 300;
    default:
        return ui_text_w(F_SMALL, r->v) > RIGHT - 600 ? 168 : 100;
    }
}

static void draw_kv(int y0, int y1, int scroll)
{
    ui_clip(y0, y1);
    int y = y0 - scroll;
    for (int i = 0; i < nkv; ++i) {
        const struct kvr *r = &kv[i];
        int h = kv_h(r);
        if (y + h > y0 && y < y1) {
            ui_fill(0, y, UI_W, h, UI_BG);
            if (r->kind == KV_HDR) {
                ui_text(F_TAB, PAD, y + 80, r->k, UI_HOLO, UI_BG);
                ui_fill(PAD - 8, y + h - 8, UI_W - 2 * PAD + 16, 3, UI_HOLO_DIM);
            } else if (r->kind == KV_MONO) {
                ui_text(F_MONO, 28, y + 34, r->v, r->c, UI_BG);
            } else if (r->kind == KV_BIG) {
                ui_text_c(F_BIG, UI_W / 2, y + 170, r->v, r->c, UI_BG);
                ui_text_c(F_SMALL, UI_W / 2, y + 250, r->k, UI_TEXT2, UI_BG);
            } else if (h > 100) {
                ui_text(F_SMALL, PAD, y + 62, r->k, UI_TEXT2, UI_BG);
                ui_text(F_SMALL, PAD + 40, y + 128, r->v, r->c, UI_BG);
                ui_fill(PAD, y + h - 2, UI_W - 2 * PAD, 2, UI_DIV);
            } else {
                ui_text(F_SMALL, PAD, y + 64, r->k, UI_TEXT2, UI_BG);
                ui_text_r(F_SMALL, RIGHT, y + 64, r->v, r->c, UI_BG);
                ui_fill(PAD, y + h - 2, UI_W - 2 * PAD, 2, UI_DIV);
            }
        }
        y += h;
    }
    if (y < y1)
        ui_fill(0, y, UI_W, y1 - y, UI_BG);
    ui_clip(0, UI_H);
}

static int kv_total(void)
{
    int t = 0;
    for (int i = 0; i < nkv; ++i)
        t += kv_h(&kv[i]);
    return t + 40;
}

/* An info screen: fill() builds kv[]; refresh_ms > 0 rebuilds it live. */
static void info_screen(const char *title, void (*fill)(void), unsigned refresh_ms)
{
    int scroll = 0;
    nkv = 0;
    fill();
    draw_status();
    draw_bar(title, 1);
    draw_nav();
    draw_kv(PAGE_Y0, NAV_Y, 0);
    for (;;) {
        struct ev e;
        ev_wait(&e, refresh_ms);
        int view = NAV_Y - PAGE_Y0, old = scroll;
        if (e.type == EV_NONE) {
            nkv = 0;
            fill();
            draw_kv(PAGE_Y0, NAV_Y, scroll);
            continue;
        }
        if (e.type == EV_BACK || e.type == EV_SELECT)
            return;
        if (e.type == EV_HOME) {
            E.home = 1;
            return;
        }
        if (e.type == EV_TAP) {
            int c = chrome_tap(&e, 1);
            if (c == 1 || c == 3)
                return;
            if (c == 2) {
                E.home = 1;
                return;
            }
            continue;
        }
        if (e.type == EV_DOWN)
            scroll += 600;
        else if (e.type == EV_UP)
            scroll -= 600;
        else if (e.type == EV_DRAG)
            scroll -= e.d;
        scroll = clamp_scroll(scroll, kv_total(), view);
        if (scroll != old)
            draw_kv(PAGE_Y0, NAV_Y, scroll);
    }
}

/* A list screen: build() fills rows[]; act(i) runs a row (returns 1 to
 * close the screen). Rebuilt after every action. */
static void list_screen(const char *title, void (*build)(void), int (*act)(int))
{
    int focus = -1, scroll = 0;
    nrows = 0;
    build();
    focus = next_enabled(-1, 1);
    draw_status();
    draw_bar(title, 1);
    draw_nav();
    draw_rows(PAGE_Y0, NAV_Y, scroll, focus);
    for (;;) {
        struct ev e;
        int view = NAV_Y - PAGE_Y0, pick = -1;
        ev_wait(&e, 0);
        if (e.type == EV_BACK)
            return;
        if (e.type == EV_HOME) {
            E.home = 1;
            return;
        }
        if (e.type == EV_UP || e.type == EV_DOWN) {
            focus = next_enabled(focus, e.type == EV_UP ? -1 : 1);
            scroll = keep_visible(focus, scroll, view);
        } else if (e.type == EV_DRAG) {
            scroll = clamp_scroll(scroll - e.d, rows_total(), view);
        } else if (e.type == EV_SELECT) {
            pick = focus;
        } else if (e.type == EV_TAP) {
            int c = chrome_tap(&e, 1);
            if (c == 1 || c == 3)
                return;
            if (c == 2) {
                E.home = 1;
                return;
            }
            int i = row_at(PAGE_Y0, scroll, e.y);
            if (i >= 0 && !(rows[i].f & R_DIS)) {
                focus = i;
                pick = i;
            }
        }
        if (pick >= 0 && pick < nrows && !(rows[pick].f & R_DIS)) {
            draw_rows(PAGE_Y0, NAV_Y, scroll, pick);
            if (act(pick))
                return;
            if (E.home || E.boot)
                return;
            nrows = 0;
            build();
            draw_status();
            draw_bar(title, 1);
            draw_nav();
        }
        draw_rows(PAGE_Y0, NAV_Y, scroll, focus);
    }
}

/* ---- monospace text viewer (logs, command line, hex) ---------------------- */

#define TV_CAP 24000
#define TV_LINES 1200
#define TV_COLS 74

static char tv_buf[TV_CAP];
static uint32_t tv_off[TV_LINES];
static int tv_n;
static uint32_t tv_len;

static void tv_reset(void)
{
    tv_n = 0;
    tv_len = 0;
}

/* Append one line (wrapped at TV_COLS). */
static void tv_line(const char *s)
{
    unsigned n = ui_strlen(s);
    unsigned pos = 0;
    do {
        if (tv_n >= TV_LINES || tv_len + TV_COLS + 2 >= TV_CAP)
            return;
        tv_off[tv_n++] = tv_len;
        unsigned k = 0;
        for (; k < TV_COLS && pos < n; ++k)
            tv_buf[tv_len++] = s[pos++];
        tv_buf[tv_len++] = 0;
    } while (pos < n);
}

static void draw_tv(int y0, int y1, int scroll)
{
    const int lh = 46;
    ui_clip(y0, y1);
    ui_fill(0, y0, UI_W, y1 - y0, UI_BG);
    int first = scroll / lh;
    for (int i = first; i < tv_n; ++i) {
        int y = y0 + i * lh - scroll;
        if (y >= y1)
            break;
        ui_text(F_MONO, 24, y + 34, tv_buf + tv_off[i], UI_TEXT, UI_BG);
    }
    ui_clip(0, UI_H);
}

static void text_screen(const char *title)
{
    int scroll = 0;
    const int lh = 46, view = NAV_Y - PAGE_Y0;
    draw_status();
    draw_bar(title, 1);
    draw_nav();
    draw_tv(PAGE_Y0, NAV_Y, 0);
    for (;;) {
        struct ev e;
        int old = scroll;
        ev_wait(&e, 0);
        if (e.type == EV_BACK || e.type == EV_SELECT)
            return;
        if (e.type == EV_HOME) {
            E.home = 1;
            return;
        }
        if (e.type == EV_TAP) {
            int c = chrome_tap(&e, 1);
            if (c == 1 || c == 3)
                return;
            if (c == 2) {
                E.home = 1;
                return;
            }
            continue;
        }
        if (e.type == EV_DOWN)
            scroll += view - lh * 2;
        else if (e.type == EV_UP)
            scroll -= view - lh * 2;
        else if (e.type == EV_DRAG)
            scroll -= e.d;
        scroll = clamp_scroll(scroll, tv_n * lh + lh, view);
        if (scroll != old)
            draw_tv(PAGE_Y0, NAV_Y, scroll);
    }
}

/* ---- S-Boot command line ---------------------------------------------------- */

/* Copy the value of "key=" from S-Boot's bootargs; returns 0 if absent. */
static int bootarg(const char *key, char *out, unsigned cap)
{
    out[0] = 0;
    if (!E.args)
        return 0;
    unsigned kl = ui_strlen(key);
    const volatile char *a = (const volatile char *)E.args;
    uint32_t n = E.args_len;
    for (uint32_t i = 0; i < n;) {
        while (i < n && (a[i] == ' ' || a[i] == 0))
            ++i;
        uint32_t s = i;
        while (i < n && a[i] != ' ' && a[i] != 0)
            ++i;
        if (i - s > kl && a[s + kl] == '=') {
            unsigned m = 0;
            while (m < kl && a[s + m] == key[m])
                ++m;
            if (m == kl) {
                unsigned o = 0;
                for (uint32_t j = s + kl + 1; j < i && o + 1 < cap; ++j)
                    out[o++] = a[j];
                out[o] = 0;
                return 1;
            }
        }
    }
    return 0;
}

static void kv_arg(const char *label, const char *key)
{
    char v[96];
    if (bootarg(key, v, sizeof(v)))
        kv_add(label, v);
    else
        kv_addc(label, "-", UI_TEXT3);
}

/* ======================================================================= */
/* DEVICE                                                                   */
/* ======================================================================= */

/* This boot's intro (anim.c star_anim_stats; also log page slots 112..114): frames, the longest
 * frame and the time the loader and the menu took between the intro and the reveal. */
extern uint32_t star_anim_stats[8];

static void anim_line(const char *k, uint32_t frames, uint32_t ms, uint32_t worst, uint32_t flags)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, frames);
    ub_s(&u, " frames in ");
    ub_dec(&u, ms);
    ub_s(&u, " ms, worst ");
    ub_dec(&u, worst);
    ub_s(&u, " ms");
    if (ms && frames > 1) {
        ub_s(&u, ", ");
        ub_dec(&u, (uint64_t)(frames - 1) * 1000U / ms);
        ub_s(&u, " fps");
    }
    if (flags & 2U)
        ub_s(&u, ", skipped");
    if (flags & 4U)
        ub_s(&u, ", plain base");
    kv_add(k, u.b);
}

static void fill_anim_stats(void)
{
    const uint32_t *a = star_anim_stats;
    if (a[7] & 1U) {
        anim_line("Boot intro", a[0], a[2], a[1], a[7]);
        kv_dec("Intro to menu", a[3], " ms (loader and menu work)");
    }
    if (a[7] & 8U)
        anim_line("Menu reveal", a[4], a[6], a[5], a[7] & 16U ? 2U : 0U);
}

static void fill_device_info(void)
{
    char v[96];
    kv_hdr("PRODUCT");
    kv_arg("Model", "androidboot.em.model");
    kv_add("Device", "Samsung Galaxy S9 (starlte)");
    kv_arg("Hardware revision", "androidboot.revision");
    kv_arg("Serial number", "androidboot.serialno");
    kv_arg("AP serial", "androidboot.ap_serial");
    kv_arg("Sales code", "sales_code");
    kv_arg("Carrier ID", "androidboot.carrierid");
    kv_arg("Region", "region1");
    kv_hdr("BOOTLOADER");
    kv_arg("S-Boot version", "androidboot.bootloader");
    kv_arg("Warranty bit (Knox)", "androidboot.warranty_bit");
    kv_arg("Verified boot state", "androidboot.verifiedbootstate");
    kv_arg("FMM lock", "androidboot.fmm_lock");
    kv_arg("Odin download flag", "androidboot.odin_download");
    kv_arg("HMAC mismatch", "androidboot.hmac_mismatch");
    kv_arg("Boot mode", "bootmode");
    kv_arg("Charging mode", "charging_mode");
    kv_hdr("HARDWARE");
    kv_arg("DRAM", "androidboot.dram_info");
    if (bootarg("sec_debug.lpddr4_size", v, sizeof(v))) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, v);
        ub_s(&u, " GB LPDDR4X");
        kv_add("Memory", u.b);
    }
    kv_arg("LCD type", "lcdtype");
    kv_arg("PMIC info", "pmic_info");
    kv_arg("CCIC info", "ccic_info");
    kv_hdr("STARBITE");
    kv_add("Boot menu", "v8 (Starred Wordmark, touch)");
    kv_add("EngineerMode", "1.2");
    fill_anim_stats();
}

/* ---- trapped reads (em_vec.S) ---------------------------------------------- */

extern char em_vectors[], em_core_entry[];
extern uint64_t em_fault[6], em_core_box[32];
#define EMF ((volatile uint64_t *)em_fault)

/* System register read that survives a trap: 1 = read, 0 = trapped. */
#define SAFE_MRS(reg, out)                                                  \
    ({                                                                      \
        uint64_t _c = EMF[3], _v;                                           \
        __asm__ volatile("mov %0, xzr\n\tmrs %0, " #reg : "=&r"(_v) : : "memory"); \
        *(out) = _v;                                                        \
        EMF[3] == _c;                                                       \
    })

/* 32-bit load that survives a synchronous abort: 1 = read, 0 = faulted. */
static int probe32(uintptr_t a, uint32_t *v)
{
    uint64_t c = EMF[3];
    uint32_t x;
    __asm__ volatile("mov %w0, wzr\n\tldr %w0, [%1]\n\tdsb sy" : "=&r"(x) : "r"(a) : "memory");
    *v = x;
    return EMF[3] == c;
}

static void kv_trap(const char *k)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, "trapped, ESR ");
    ub_hex(&u, EMF[0], 8);
    kv_addc(k, u.b, UI_ORANGE);
}

static void kv_sysreg(const char *k, int ok, uint64_t v)
{
    if (ok)
        kv_hex(k, v, 16);
    else
        kv_trap(k);
}

/* CHIPID block 0x10000000: PRO_ID at +0x0, unique ID at +0x14 / +0x18. */
static void fill_soc(void)
{
    uint64_t midr, mpidr, cel, isar0 = 0, pfr0 = 0, mmfr0 = 0, ctr = 0, clidr = 0, cntfrq;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(cel));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    int ok_isar = SAFE_MRS(id_aa64isar0_el1, &isar0);
    int ok_pfr = SAFE_MRS(id_aa64pfr0_el1, &pfr0);
    int ok_mmfr = SAFE_MRS(id_aa64mmfr0_el1, &mmfr0);
    int ok_ctr = SAFE_MRS(ctr_el0, &ctr);
    int ok_clidr = SAFE_MRS(clidr_el1, &clidr);
    uint32_t pro = 0, u0 = 0, u1 = 0;
    int ok_pro = probe32(0x10000000UL, &pro);
    int ok_uid = probe32(0x10000014UL, &u0) && probe32(0x10000018UL, &u1);
    struct ub u;

    kv_hdr("SYSTEM ON CHIP");
    kv_add("SoC", "Samsung Exynos 9810 (10 nm LPP)");
    if (ok_pro) {
        kv_hex("CHIPID PRO_ID", pro, 8);
        ub_init(&u);
        ub_s(&u, "product ");
        ub_hexn(&u, pro >> 12, 5);
        ub_s(&u, ", rev ");
        ub_dec(&u, (pro >> 4) & 0xfU);
        ub_c(&u, '.');
        ub_dec(&u, pro & 0xfU);
        kv_add("Decoded", u.b);
    } else {
        kv_trap("CHIPID PRO_ID");
    }
    if (ok_uid) {
        ub_init(&u);
        ub_hexn(&u, u1 & 0xffffU, 4);
        ub_hexn(&u, u0, 8);
        kv_add("Unique ID", u.b);
    } else {
        kv_trap("Unique ID");
    }
    kv_add("CPU cores", "4x Mongoose M3 + 4x Cortex-A55");
    kv_add("GPU", "Mali-G72 MP18");

    kv_hdr("THIS CPU (BOOT CORE)");
    ub_init(&u);
    unsigned part = (unsigned)(midr >> 4) & 0xfffU, impl = (unsigned)(midr >> 24) & 0xffU;
    if (impl == 0x41 && part == 0xd05)
        ub_s(&u, "ARM Cortex-A55");
    else if (impl == 0x53)
        ub_s(&u, "Samsung Mongoose");
    else
        ub_s(&u, "unknown");
    ub_s(&u, " r");
    ub_dec(&u, (midr >> 20) & 0xfU);
    ub_c(&u, 'p');
    ub_dec(&u, midr & 0xfU);
    kv_add("Core", u.b);
    kv_hex("MIDR_EL1", midr, 8);
    kv_hex("MPIDR_EL1", mpidr, 8);
    kv_dec("Exception level", (cel >> 2) & 3U, 0);
    kv_dec("CNTFRQ_EL0", cntfrq, cntfrq ? " Hz" : " (S-Boot left 0; 26 MHz)");

    kv_hdr("ID REGISTERS");
    kv_sysreg("ID_AA64ISAR0_EL1", ok_isar, isar0);
    if (ok_isar) {
        ub_init(&u);
        if ((isar0 >> 4) & 0xfU)
            ub_s(&u, "AES ");
        if ((isar0 >> 8) & 0xfU)
            ub_s(&u, "SHA1 ");
        if ((isar0 >> 12) & 0xfU)
            ub_s(&u, "SHA2 ");
        if ((isar0 >> 16) & 0xfU)
            ub_s(&u, "CRC32 ");
        if ((isar0 >> 20) & 0xfU)
            ub_s(&u, "LSE ");
        if ((isar0 >> 28) & 0xfU)
            ub_s(&u, "RDM ");
        if ((isar0 >> 44) & 0xfU)
            ub_s(&u, "DotProd");
        kv_add("ISA extensions", u.b);
    }
    kv_sysreg("ID_AA64PFR0_EL1", ok_pfr, pfr0);
    if (ok_pfr) {
        ub_init(&u);
        ub_s(&u, (((pfr0 >> 8) & 0xfU) ? "EL2 " : "no-EL2 "));
        ub_s(&u, (((pfr0 >> 12) & 0xfU) ? "EL3 " : ""));
        ub_s(&u, (((pfr0 >> 16) & 0xfU) == 0xf) ? "no-FP" : "FP");
        ub_s(&u, (((pfr0 >> 20) & 0xfU) == 0xf) ? " no-SIMD" : " SIMD");
        kv_add("Features", u.b);
    }
    kv_sysreg("ID_AA64MMFR0_EL1", ok_mmfr, mmfr0);
    if (ok_mmfr) {
        static const unsigned char pa[7] = { 32, 36, 40, 42, 44, 48, 52 };
        unsigned par = (unsigned)mmfr0 & 0xfU;
        kv_dec("Physical address bits", par < 7 ? pa[par] : 0, 0);
    }
    kv_sysreg("CTR_EL0", ok_ctr, ctr);
    if (ok_ctr)
        kv_dec("D-cache line", 4U << ((ctr >> 16) & 0xfU), " bytes");
    kv_sysreg("CLIDR_EL1", ok_clidr, clidr);

    kv_hdr("CACHES");
    for (unsigned lvl = 0; ok_clidr && lvl < 3; ++lvl) {
        unsigned ty = (unsigned)(clidr >> (3 * lvl)) & 7U;
        if (!ty)
            break;
        uint64_t sel = (uint64_t)lvl << 1, ccs = 0, c = EMF[3];
        __asm__ volatile("mov %0, xzr\n\tmsr csselr_el1, %1\n\tisb\n\tmrs %0, ccsidr_el1"
                         : "=&r"(ccs) : "r"(sel) : "memory");
        const char *name = lvl == 0 ? "L1 D-cache" : lvl == 1 ? "L2 cache" : "L3 cache";
        if (EMF[3] != c) {
            kv_trap(name);
            continue;
        }
        uint64_t line = 16ULL << (ccs & 7U), ways = ((ccs >> 3) & 0x3ffU) + 1, sets = ((ccs >> 13) & 0x7fffU) + 1;
        ub_init(&u);
        ub_size(&u, line * ways * sets);
        ub_s(&u, ", ");
        ub_dec(&u, ways);
        ub_s(&u, "-way");
        kv_add(name, u.b);
    }
    if (EMF[3]) {
        kv_hdr("TRAPS (CAUGHT BY ENGINEERMODE)");
        kv_dec("Count", EMF[3], 0);
        kv_hex("Last ESR", EMF[0], 8);
        kv_hex("Last ELR", EMF[1], 16);
    }
}

static void fill_memory(void)
{
    uint64_t base[8], size[8], total = 0;
    int n = fdt_memory_banks(E.in ? E.in->fdt : 0, base, size, 8);
    struct ub u;
    kv_hdr("DRAM BANKS (S-BOOT DEVICE TREE)");
    for (int i = 0; i < n; ++i) {
        ub_init(&u);
        ub_hex(&u, base[i], 9);
        ub_s(&u, " + ");
        ub_size(&u, size[i]);
        kv_add("Bank", u.b);
        total += size[i];
    }
    if (!n)
        kv_addc("Bank", "none reported", UI_TEXT3);
    ub_init(&u);
    ub_size(&u, total);
    kv_add("Total", u.b);
    kv_hdr("STARBITE LAYOUT");
    kv_add("Loader + payload", "0x080080000");
    kv_add("RAM test area", "0x092000000 + 16 MiB");
    kv_add("Screen save", "0x094000000 + 17 MiB");
    kv_add("Windows UEFI (FD)", "0x090000000 + 7 MiB");
    kv_add("UFS DMA (loader)", "0x0AF000000 + 1 MiB");
    kv_add("Linux DTB (OS store)", "0x0AE000000");
    if (E.in) {
        ub_init(&u);
        ub_hex(&u, E.in->phys_base, 9);
        ub_s(&u, " + ");
        ub_size(&u, E.in->mem_size);
        kv_add("iOS (XNU) RAM", u.b);
    }
    kv_add("Windows RAM", "3.4 GB (stage 3)");
    kv_hdr("CARVE-OUTS (VENDOR DTB)");
    kv_add("RKP", "0x0AF800000 + 32 MiB");
    kv_add("TIMA", "0x0B8000000 + 2 MiB");
    kv_add("Secure world", "0x0BC800000 .. 0x0C0000000");
    kv_add("seclog", "0x0C0000000 + 512 KiB");
    kv_add("Framebuffer", "0x0CC000000 + 16.3 MiB");
    kv_add("Secure camera", "0x0E1900000 + 30 MiB");
    kv_add("Modem (CP)", "0x0F0000000 + 146 MiB");
    kv_add("CP logging", "0x0F9200000 + 32 MiB");
    kv_add("Audio (abox)", "0x0FB200000 + 40 MiB");
    kv_add("exynos-ss + pstore", "0x0FDA00000 + 28 MiB");
    kv_add("Camera firmware", "0x0FF800000 + 8 MiB");
}

static void rst_bits(struct ub *u, uint32_t v)
{
    if (v & 0x20000000U)
        ub_s(u, "SWRESET ");
    if (v & 0x01000000U)
        ub_s(u, "CL0-WDT ");
    if (v & 0x02000000U)
        ub_s(u, "CL1-WDT ");
    if (v & 0x00010000U)
        ub_s(u, "PIN ");
    if (!u->n)
        ub_s(u, "none decoded");
}

static void fill_reset(void)
{
    volatile uint32_t *pmu = (volatile uint32_t *)PMU_BASE;
    uint32_t rst = pmu[0x404 / 4], i2 = pmu[0x808 / 4], i3 = pmu[0x80c / 4], sys = pmu[0x810 / 4];
    struct ub u;
    kv_hdr("PMU (LIVE)");
    kv_hex("RST_STAT", rst, 8);
    ub_init(&u);
    rst_bits(&u, rst);
    kv_add("Reset source", u.b);
    kv_hex("INFORM2", i2, 8);
    kv_hex("INFORM3", i3, 8);
    kv_add("INFORM3 means", i3 == INFORM_RECOVERY ? "recovery next" : i3 == INFORM_DOWNLOAD ? "download next" :
                           i3 == INFORM_NORMAL ? "normal boot" : "other");
    kv_hex("SYSIP_DAT0", sys, 8);
    kv_hdr("S-BOOT REPORT");
    kv_arg("sec_debug.rst_stat", "sec_debug.rst_stat");
    kv_arg("Reset reason", "sec_debug.reset_reason");
    kv_arg("Power-on source", "sec_debug.pwronsrc");
    kv_arg("Power-off source", "sec_debug.pwroffsrc");
    kv_arg("Boot mode", "bootmode");
    kv_hdr("PMIC");
    uint8_t on = 0, off = 0, s1 = 0, s2 = 0;
    if (hw_pmic_read(1, 0x10, &on))
        kv_hex("PWRONSRC", on, 2);
    if (hw_pmic_read(1, 0x11, &off))
        kv_hex("OFFSRC", off, 2);
    if (hw_pmic_read(1, 0x0e, &s1))
        kv_hex("STATUS1", s1, 2);
    if (hw_pmic_read(1, 0x0f, &s2))
        kv_hex("STATUS2", s2, 2);
}

/* ======================================================================= */
/* HARDWARE TESTING                                                          */
/* ======================================================================= */

static void fill_cpu(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    kv_hdr("CLUSTERS");
    kv_add("Cluster 0", "4x Cortex-A55, up to 1.95 GHz");
    kv_add("Cluster 1", "4x Mongoose M3, up to 2.7 GHz");
    kv_hdr("CORE MAP (MPIDR: MT, AFF2 CLUSTER, AFF1 CORE)");
    for (unsigned c = 0; c < 8; ++c) {
        struct ub k, v;
        uint64_t aff = 0x81000000ULL | (c < 4 ? 0 : 0x10000U) | ((uint64_t)(c & 3U) << 8);
        ub_init(&k);
        ub_s(&k, "cpu");
        ub_dec(&k, c);
        ub_s(&k, c < 4 ? "  A55" : "  M3");
        ub_init(&v);
        ub_hex(&v, aff, 8);
        ub_s(&v, c < 4 ? "  PSCI " : "  PSCI ");
        ub_hex(&v, c < 4 ? c : 0x100U + (c - 4U), 3);
        int here = (mpidr & 0xffffffU) == (aff & 0xffffffU);
        ub_s(&v, here ? "  running here" : "");
        kv_addc(k.b, v.b, here ? UI_GREEN : UI_TEXT2);
    }
    kv_hdr("NOTES");
    kv_add("Bootloader", "single core, MMU off");
    kv_add("iOS", "cpu0-3 (A55) via PSCI CPU_ON 1-3");
    kv_add("Windows", "4 cores (numproc 4)");
    kv_add("M3 MPIDR", "expected value; see the bring-up test");
}

/* Fullscreen colour fields; tap or Power: next, Bixby: leave. */
static void test_display(void)
{
    static const uint32_t col[6] = { 0xffff0000U, 0xff00ff00U, 0xff0000ffU, 0xffffffffU, 0xff000000U, 0xff808080U };
    for (unsigned f = 0; f < 9; ++f) {
        ui_clip(0, UI_H);
        if (f < 6) {
            ui_fill(0, 0, UI_W, UI_H, col[f]);
        } else if (f == 6) {
            for (int y = 0; y < UI_H; y += 8)
                ui_fill(0, y, UI_W, 8, 0xff000000U | (uint32_t)(y * 255 / UI_H) * 0x010101U);
        } else if (f == 7) {
            for (int y = 0; y < UI_H; y += 80)
                for (int x = 0; x < UI_W; x += 80)
                    ui_fill(x, y, 80, 80, ((x / 80 + y / 80) & 1) ? UI_TEXT : UI_BLACK);
        } else {
            ui_fill(0, 0, UI_W, UI_H, UI_BLACK);
            for (int x = 0; x <= UI_W; x += 144)
                ui_fill(x >= UI_W ? UI_W - 2 : x, 0, 2, UI_H, UI_HOLO);
            for (int y = 0; y <= UI_H; y += 148)
                ui_fill(0, y >= UI_H ? UI_H - 2 : y, UI_W, 2, UI_HOLO);
        }
        struct ub u;
        ub_init(&u);
        ub_s(&u, "FIELD ");
        ub_dec(&u, f + 1);
        ub_s(&u, "/9  ");
        ub_s(&u, UI_DOT);
        ub_s(&u, "  TAP OR POWER: NEXT  ");
        ub_s(&u, UI_DOT);
        ub_s(&u, "  BIXBY: BACK");
        uint32_t bg = f < 6 ? col[f] : UI_BLACK;
        uint32_t fg = (f == 3 || f == 1) ? UI_BLACK : (f == 6 ? UI_HOLO : 0xffd0d0d0U);
        ui_text_c(F_MONO, UI_W / 2, UI_H - 120, u.b, fg, f == 6 ? 0xff000000U | 0xf0f0f0U : bg);
        for (;;) {
            struct ev e;
            ev_wait(&e, 0);
            if (e.type == EV_BACK || e.type == EV_HOME)
                return;
            if (e.type == EV_SELECT || e.type == EV_TAP || e.type == EV_DOWN || e.type == EV_UP)
                break;
        }
    }
}

/* Paint with one finger; Power clears, Bixby leaves. */
static void test_touch(void)
{
    uint8_t id[3] = { 0, 0, 0 }, pi[11];
    unsigned mx, my;
    hw_tp_start();
    for (;;) {
        ui_clip(0, UI_H);
        ui_fill(0, 0, UI_W, UI_H, UI_BLACK);
        for (int x = 0; x < UI_W; x += 144)
            ui_fill(x, 0, 1, UI_H, 0xff1a1e26U);
        for (int y = 0; y < UI_H; y += 148)
            ui_fill(0, y, UI_W, 1, 0xff1a1e26U);
        struct ub u;
        ub_init(&u);
        if (!hw_tp_ok()) {
            ui_text_c(F_BODY, UI_W / 2, 1400, "Touch controller did not start", UI_RED, UI_BLACK);
        } else {
            hw_tp_max(&mx, &my);
            (void)hw_tp_read(0x52, id, 3);
            ub_s(&u, "S6SY761 ");
            ub_hexn(&u, id[0], 2);
            ub_hexn(&u, id[1], 2);
            ub_hexn(&u, id[2], 2);
            ub_s(&u, "  panel ");
            ub_dec(&u, mx);
            ub_c(&u, 'x');
            ub_dec(&u, my);
            if (hw_tp_read(0x23, pi, 11)) {
                ub_s(&u, "  ");
                ub_dec(&u, pi[8]);
                ub_c(&u, 'x');
                ub_dec(&u, pi[9]);
                ub_s(&u, " ch");
            }
            ui_text(F_MONO, 30, 60, u.b, UI_HOLO, UI_BLACK);
        }
        ui_text_c(F_MONO, UI_W / 2, UI_H - 60, "DRAW  " UI_DOT "  POWER: CLEAR  " UI_DOT "  BIXBY: BACK", 0xffa0a0a0U, UI_BLACK);
        int lx = -1, ly = -1;
        for (;;) {
            unsigned k = hw_keys();
            if (k & K_BIXBY) {
                while (hw_keys())
                    hw_wait_ms(20);
                held_keys = 0;
                return;
            }
            if (k & K_POWER) {
                while (hw_keys())
                    hw_wait_ms(20);
                break;
            }
            int st, x, y;
            while (hw_tp_event(&st, &x, &y)) {
                if (st == 3) {
                    lx = -1;
                    continue;
                }
                if (lx >= 0) {
                    int steps = iabs(x - lx) > iabs(y - ly) ? iabs(x - lx) : iabs(y - ly);
                    steps = steps / 4 + 1;
                    for (int s = 0; s <= steps; ++s)
                        ui_fill(lx + (x - lx) * s / steps - 5, ly + (y - ly) * s / steps - 5, 11, 11, 0xff7cf0b4U);
                } else {
                    ui_fill(x - 8, y - 8, 17, 17, 0xff7cf0b4U);
                }
                lx = x;
                ly = y;
                ub_init(&u);
                ub_s(&u, "x ");
                ub_dec(&u, (uint64_t)x);
                ub_s(&u, "  y ");
                ub_dec(&u, (uint64_t)y);
                ub_s(&u, st == 1 ? "  down" : "  move");
                ui_fill(0, 90, UI_W, 50, UI_BLACK);
                ui_text(F_MONO, 30, 128, u.b, UI_TEXT, UI_BLACK);
            }
            hw_wait_ms(4);
        }
    }
}

static void fill_keys(void)
{
    unsigned k = hw_keys();
    uint32_t a = *(volatile uint32_t *)0x14050024UL, b = *(volatile uint32_t *)0x14050064UL;
    kv_hdr("LIVE (PRESS ANY KEY " UI_DOT " HOLD BIXBY TO LEAVE)");
    kv_addc("Volume up   (GPA0-3)", (k & K_UP) ? "PRESSED" : "released", (k & K_UP) ? UI_GREEN : UI_TEXT2);
    kv_addc("Volume down (GPA0-4)", (k & K_DOWN) ? "PRESSED" : "released", (k & K_DOWN) ? UI_GREEN : UI_TEXT2);
    kv_addc("Bixby       (GPA0-6)", (k & K_BIXBY) ? "PRESSED" : "released", (k & K_BIXBY) ? UI_GREEN : UI_TEXT2);
    kv_addc("Power       (GPA2-4)", (k & K_POWER) ? "PRESSED" : "released", (k & K_POWER) ? UI_GREEN : UI_TEXT2);
    kv_hdr("RAW");
    kv_hex("GPA0 DAT", a, 8);
    kv_hex("GPA2 DAT", b, 8);
    kv_addc("Touch INT  (GPA1-0)", (*(volatile uint32_t *)0x14050044UL & 1U) ? "high (idle)" : "LOW (event)",
            UI_TEXT2);
}

/* Keys page: its own loop (keys are the thing under test). */
static void test_keys(void)
{
    draw_status();
    draw_bar("Keys", 1);
    draw_nav();
    unsigned held = 0;
    uint64_t t0 = 0;
    for (;;) {
        nkv = 0;
        fill_keys();
        draw_kv(PAGE_Y0, NAV_Y, 0);
        unsigned k = hw_keys();
        if (k == K_BIXBY) {
            if (!held)
                t0 = hw_ticks();
            held = 1;
            if (hw_ticks() - t0 > hw_freq())
                break;
        } else {
            held = 0;
        }
        int st, x, y;
        if (hw_tp_event(&st, &x, &y) && st == 3 && y >= NAV_Y)
            break;
        hw_wait_ms(40);
    }
    while (hw_keys())
        hw_wait_ms(20);
    held_keys = 0;
}

#define RAMTEST_BASE 0x92000000UL
#define RAMTEST_SIZE (16UL << 20)

static uint64_t ram_errors, ram_ms;
static int ram_done;

static void fill_ram(void)
{
    struct ub u;
    kv_hdr("PATTERN TEST");
    kv_add("Region", "0x092000000 + 16 MiB (free)");
    kv_add("Passes", "address pattern, inverted");
    if (!ram_done) {
        kv_add("Status", "not run: press Power on the list");
        return;
    }
    kv_addc("Result", ram_errors ? "FAIL" : "PASS: no errors", ram_errors ? UI_RED : UI_GREEN);
    kv_dec("Bad words", ram_errors, 0);
    kv_dec("Time", ram_ms, " ms");
    ub_init(&u);
    ub_dec(&u, ram_ms ? (RAMTEST_SIZE * 4 / 1024 / 1024) * 1000 / ram_ms : 0);
    ub_s(&u, " MiB/s (uncached, MMU off)");
    kv_add("Throughput", u.b);
}

static void run_ram(void)
{
    volatile uint64_t *m = (volatile uint64_t *)RAMTEST_BASE;
    uint64_t n = RAMTEST_SIZE / 8, t0 = hw_ticks();
    ram_errors = 0;
    draw_status();
    draw_bar("Memory test", 1);
    draw_nav();
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint64_t inv = pass ? ~0ULL : 0;
        nkv = 0;
        kv_hdr("RUNNING");
        kv_add("Pass", pass ? "2/2 inverted pattern" : "1/2 address pattern");
        draw_kv(PAGE_Y0, NAV_Y, 0);
        for (uint64_t i = 0; i < n; ++i)
            m[i] = (i * 0x9e3779b97f4a7c15ULL) ^ inv;
        __asm__ volatile("dsb sy" : : : "memory");
        for (uint64_t i = 0; i < n; ++i)
            if (m[i] != ((i * 0x9e3779b97f4a7c15ULL) ^ inv))
                ++ram_errors;
    }
    ram_ms = (hw_ticks() - t0) * 1000U / hw_freq();
    ram_done = 1;
    info_screen("Memory test", fill_ram, 0);
}

static void fill_rtc(void)
{
    struct rtc r;
    struct ub u;
    rtc_read(&r);
    ub_init(&u);
    if (r.ok) {
        ub_dec2(&u, r.h);
        ub_c(&u, ':');
        ub_dec2(&u, r.mi);
        ub_c(&u, ':');
        ub_dec2(&u, r.s);
    } else {
        ub_s(&u, "--:--:--");
    }
    struct ub d;
    ub_init(&d);
    if (r.ok) {
        ub_dec(&d, r.y);
        ub_c(&d, '-');
        ub_dec2(&d, r.mo);
        ub_c(&d, '-');
        ub_dec2(&d, r.d);
        ub_s(&d, "  UTC");
    } else {
        ub_s(&d, "RTC not readable");
    }
    kv_big(d.b, u.b);
    kv_hdr("S2MPS18 RTC (ACPM TYPE 2)");
    kv_hex("CTRL", r.ctrl, 2);
    kv_add("Mode", (r.ctrl & 2U) ? "24 h, binary" : "12 h or BCD");
    kv_add("Used by", "Windows clock (UEFI), TWRP, iOS later");
    kv_add("Set from", "TWRP: date -u -s, hwclock -w -u");
}

static uint64_t hz_measured;

static void fill_timer(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    kv_hdr("GENERIC TIMER");
    kv_dec("CNTFRQ_EL0", f, f ? " Hz" : " (not set by S-Boot)");
    kv_dec("Assumed", hw_freq(), " Hz");
    kv_hex("CNTPCT_EL0", hw_ticks(), 16);
    struct ub u;
    ub_init(&u);
    ub_dec(&u, hw_ticks() / hw_freq());
    ub_s(&u, " s");
    kv_big("since power-on (counter)", u.b);
    if (hz_measured)
        kv_dec("Counter vs RTC", hz_measured, " ticks per RTC second");
}

/* Count counter ticks across one PMIC RTC second boundary. */
static void measure_timer(void)
{
    struct rtc a, b;
    rtc_read(&a);
    if (!a.ok)
        return;
    uint64_t t0 = 0;
    for (unsigned i = 0; i < 400; ++i) {
        rtc_read(&b);
        if (b.ok && b.s != a.s) {
            if (!t0) {
                t0 = hw_ticks();
                a = b;
            } else {
                hz_measured = hw_ticks() - t0;
                return;
            }
        }
        hw_wait_ms(5);
    }
}

/* ======================================================================= */
/* POWER                                                                      */
/* ======================================================================= */

static void ldo_name(struct ub *u, unsigned n)
{
    ub_s(u, "LDO");
    ub_dec(u, n);
    switch (n) {
    case 35:
        ub_s(u, "  tsp_io");
        break;
    case 43:
        ub_s(u, "  tsp_avdd");
        break;
    default:
        break;
    }
}

/* S2MPS18 LDOn CTRL: L1 0x49, L2 0x4A (+0x4B CTRL2), L3.. 0x4C + n - 3. */
static unsigned ldo_reg(unsigned n)
{
    return n == 1 ? 0x49U : n == 2 ? 0x4aU : 0x4cU + (n - 3U);
}

static void fill_pmic(void)
{
    char build[25];
    struct ub u;
    uint8_t id = 0;
    kv_hdr("S2MPS18 (VIA ACPM, READ ONLY)");
    if (hw_acpm_build(build))
        kv_add("ACPM firmware", build);
    else
        kv_addc("ACPM", "PMIC channel not found", UI_RED);
    if (hw_pmic_read(0, 0x00, &id))
        kv_hex("PMIC ID (TOP 0x00)", id, 2);
    kv_hdr("LDO REGULATORS: CTRL [7:6] ENABLE, [5:0] VSEL");
    for (unsigned n = 1; n <= 45; ++n) {
        uint8_t v = 0;
        struct ub k;
        ub_init(&k);
        ldo_name(&k, n);
        ub_init(&u);
        if (!hw_pmic_read(1, ldo_reg(n), &v)) {
            kv_addc(k.b, "read failed", UI_RED);
            continue;
        }
        ub_hex(&u, v, 2);
        ub_s(&u, (v & 0xc0U) == 0xc0U ? "  ON   " : (v & 0xc0U) ? "  CTRL " : "  off  ");
        ub_s(&u, "vsel ");
        ub_dec(&u, v & 0x3fU);
        if (n == 35 || n == 43) {
            ub_s(&u, "  ");
            ub_fix(&u, (n == 35 ? 700U : 1800U) + (v & 0x3fU) * 25U, 3);
            ub_s(&u, " V");
        }
        kv_addc(k.b, u.b, (v & 0xc0U) == 0xc0U ? UI_TEXT : UI_TEXT2);
    }
}

static void dump_pmic(void)
{
    struct ub u;
    tv_reset();
    tv_line("S2MPS18 PMIC block (ACPM type 1), registers 0x00-0xFF:");
    tv_line("");
    for (unsigned r = 0; r < 0x100; r += 16) {
        ub_init(&u);
        ub_hexn(&u, r, 2);
        ub_s(&u, ": ");
        for (unsigned c = 0; c < 16; ++c) {
            uint8_t v = 0;
            if (hw_pmic_read(1, r + c, &v))
                ub_hexn(&u, v, 2);
            else
                ub_s(&u, "??");
            ub_c(&u, c == 7 ? '-' : ' ');
        }
        tv_line(u.b);
    }
    tv_line("");
    tv_line("TOP block (ACPM type 0), registers 0x00-0x3F:");
    tv_line("");
    for (unsigned r = 0; r < 0x40; r += 16) {
        ub_init(&u);
        ub_hexn(&u, r, 2);
        ub_s(&u, ": ");
        for (unsigned c = 0; c < 16; ++c) {
            uint8_t v = 0;
            if (hw_pmic_read(0, r + c, &v))
                ub_hexn(&u, v, 2);
            else
                ub_s(&u, "??");
            ub_c(&u, c == 7 ? '-' : ' ');
        }
        tv_line(u.b);
    }
    text_screen("PMIC registers");
}

static void touch_rails(void)
{
    uint8_t l35 = 0, l43 = 0;
    if (!dialog("Touch rails", "Write LDO35 1.8 V and LDO43 3.0 V?", "PMIC write, same checks as the kernel", "WRITE"))
        return;
    int r = hw_touch_rails(&l35, &l43);
    struct ub u;
    ub_init(&u);
    ub_s(&u, "LDO35 ");
    ub_hex(&u, l35, 2);
    ub_s(&u, "  LDO43 ");
    ub_hex(&u, l43, 2);
    (void)dialog(r > 0 ? "Done" : r == 0 ? "Refused" : "Failed", u.b,
                 r > 0 ? "Both rails on." : r == 0 ? "Unexpected voltage: nothing written." : "ACPM did not answer.",
                 "OK");
}

/* ---- MAX77705 on HSI2C 0x14360000 (USI_CMGP03), read only ---------------
 * build/em-research-battery.md. S-Boot already drives this bus (charger,
 * fuel gauge model), so the platform writes below repeat its setup; the
 * timing registers are snapshotted before the first controller reset and
 * written back. Register reads use a repeated start like Linux (write the
 * register without STOP, then read with STOP), falling back to STOP between
 * the halves. Never writes a MAX77705 register; never reads the
 * clear-on-read interrupt registers (PMIC 0x22/0x24, charger 0xB0).
 */
#define BI 0x14360000UL
#define BI_PMIC 0x66U
#define BI_FG 0x36U
#define BI_CHG 0x69U
#define BI_USBC 0x25U

static uint32_t rd(uintptr_t a)
{
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dmb oshld" : : : "memory");
    return v;
}

static void wr(uintptr_t a, uint32_t v)
{
    __asm__ volatile("dmb oshst" : : : "memory");
    *(volatile uint32_t *)a = v;
}

static uint32_t bi_fs1, bi_fs2, bi_fs3;
static int bi_state;            /* 0 untouched, 1 up, -1 no answer */
static uint8_t bi_id, bi_rev;

static void bi_bus(void)
{
    wr(BI + 0x20, 0);
    wr(BI + 0xc4, 0);
    wr(BI + 0x00, rd(BI + 0x00) | 0x80000000U);
    wr(BI + 0x00, rd(BI + 0x00) & ~0x80000000U);
    wr(BI + 0x68, bi_fs3);
    wr(BI + 0x64, bi_fs2);
    wr(BI + 0x60, bi_fs1);
    wr(BI + 0x00, 0x8U);
    wr(BI + 0x08, 0x00ffffffU);
    wr(BI + 0x48, rd(BI + 0x48) & ~0x80000000U);
    wr(BI + 0x40, rd(BI + 0x40) | 0x80000000U);
    wr(BI + 0x24, rd(BI + 0x24) & 0xfffU);
}

/* One auto-mode message (count <= 32), with or without STOP. */
static int bi_msg(unsigned addr, int receive, uint8_t *bytes, unsigned count, int stop)
{
    int xfer_done = 0;
    unsigned done = 0, settle = 0;
    unsigned trig = count < 8 ? count : 8;
    wr(BI + 0x04, 3U | (trig << 4) | (trig << 16));
    wr(BI + 0x70, addr << 10);
    wr(BI + 0x00, 0x8U | (receive ? 0x40U : 0x80U));
    wr(BI + 0x20, 0x80U);
    wr(BI + 0x44, count | (receive ? 0x10000U : 0) | (stop ? 0x20000U : 0));
    wr(BI + 0x44, rd(BI + 0x44) | 0x80000000U);
    for (unsigned us = 0; us < 50000; ++us) {
        uint32_t irq = rd(BI + 0x24);
        if (irq)
            wr(BI + 0x24, irq);
        if (irq & 0xf00U)
            break;
        if (irq & 0x80U)
            xfer_done = 1;
        if (receive) {
            while (done < count && !(rd(BI + 0x30) & (1U << 24)))
                bytes[done++] = (uint8_t)rd(BI + 0x38);
        } else {
            while (done < count && !(rd(BI + 0x30) & (1U << 7)))
                wr(BI + 0x34, bytes[done++]);
        }
        /* Without STOP the controller may not flag TRANSFER_DONE: accept
         * an empty TX FIFO that stayed empty for 200 us. */
        if (!receive && !stop && done == count && (rd(BI + 0x30) & (1U << 8)))
            ++settle;
        if (done == count && (xfer_done || (receive && stop) || settle > 200)) {
            wr(BI + 0x20, 0);
            if (!stop)
                return 1;
            for (unsigned n = 0; n < 10000; ++n) {
                if (!(rd(BI + 0x50) & (1U << 17)))
                    return 1;
                hw_wait_us(1);
            }
            break;
        }
        hw_wait_us(1);
    }
    wr(BI + 0x20, 0);
    bi_bus();
    return 0;
}

static int bi_read(unsigned addr, uint8_t reg, uint8_t *data, unsigned len)
{
    for (unsigned a = 0; a < 2; ++a) {
        if (bi_msg(addr, 0, &reg, 1, 0) && bi_msg(addr, 1, data, len, 1))
            return 1;
        bi_bus();
        if (bi_msg(addr, 0, &reg, 1, 1) && bi_msg(addr, 1, data, len, 1))
            return 1;
        bi_bus();
        hw_wait_us(300);
    }
    return 0;
}

static int bi_init(void)
{
    if (bi_state > 0)
        return 1;
    /* CMU_CMGP gates (MANUAL + CG_VAL) and QCH for USI_CMGP03 */
    static const uint32_t gates[4] = { 0x2014, 0x2074, 0x20a8, 0x20ac };
    for (unsigned i = 0; i < 4; ++i)
        wr(0x14200000UL + gates[i], rd(0x14200000UL + gates[i]) | 0x00300000U);
    wr(0x14203050UL, rd(0x14203050UL) | 3U);
    wr(0x14212030UL, 4U);                              /* USI mode: I2C */
    for (uintptr_t g = 0x14220180UL; g <= 0x142201a0UL; g += 0x20) {   /* GPM14/15[0]: func 2, no pull */
        wr(g, (rd(g) & ~0xfU) | 0x2U);
        wr(g + 8, rd(g + 8) & ~0xfU);
    }
    __asm__ volatile("dsb sy" : : : "memory");
    if (!bi_state) {
        bi_fs1 = rd(BI + 0x60);
        bi_fs2 = rd(BI + 0x64);
        bi_fs3 = rd(BI + 0x68);
        if (!((bi_fs3 >> 16) & 0xffU)) {               /* nothing from S-Boot: 200 MHz, 400 kHz */
            bi_fs3 = (bi_fs3 & ~0x00ff0000U) | (0x21U << 16);
            bi_fs2 = (bi_fs2 & ~0xffU) | 0xe0U;
            bi_fs1 = (bi_fs1 & ~0x00ff0000U) | (0xf0U << 16);
        }
    }
    bi_bus();
    bi_state = bi_read(BI_PMIC, 0x00, &bi_id, 1) ? 1 : -1;
    if (bi_state > 0)
        (void)bi_read(BI_PMIC, 0x01, &bi_rev, 1);
    return bi_state > 0;
}

static int fg_word(uint8_t reg, uint16_t *v)
{
    uint8_t b[2];
    if (!bi_read(BI_FG, reg, b, 2))
        return 0;
    *v = (uint16_t)(b[0] | (b[1] << 8));
    return 1;
}

/* mV for VCELL-type registers: 78.125 uV per bit */
static void ub_mv(struct ub *u, uint16_t raw)
{
    ub_fix(u, (uint64_t)raw * 5U / 64U, 3);
    ub_s(u, " V");
}

static void ub_pct(struct ub *u, uint16_t raw)
{
    ub_dec(u, raw >> 8);
    ub_c(u, '.');
    ub_dec(u, (raw & 0xffU) * 10U / 256U);
    ub_s(u, " %");
}

/* Current 0x0A/0x0B: 0.3125 mA per bit with the S9's 5 mOhm sense resistor */
static void ub_ma(struct ub *u, uint16_t raw)
{
    int32_t ma = (int32_t)(int16_t)raw * 5 / 16;
    if (ma > 0)
        ub_c(u, '+');
    ub_sdec(u, ma);
    ub_s(u, " mA");
}

/* Fuel-gauge temperature: 1/256 degC per bit, shown to 0.1 */
static void ub_t256(struct ub *u, int16_t raw)
{
    int32_t t = (int32_t)raw * 10 / 256;
    if (t < 0) {
        ub_c(u, '-');
        t = -t;
    }
    ub_dec(u, (uint64_t)t / 10U);
    ub_c(u, '.');
    ub_dec(u, (uint64_t)t % 10U);
    ub_s(u, " " UI_DEG "C");
}

static const char *chg_dtls(unsigned d)
{
    switch (d) {
    case 0x0: return "pre-charge";
    case 0x1: return "fast charge (CC)";
    case 0x2: return "fast charge (CV)";
    case 0x3: return "top-off";
    case 0x4: return "done (full)";
    case 0x5: return "timer fault";
    case 0x6: return "thermal suspend";
    case 0x7: return "off (input invalid)";
    case 0x8: return "off";
    case 0xa: return "reverse boost (OTG)";
    case 0xb: return "off (watchdog expired)";
    default: return "?";
    }
}

static const char *bat_dtls(unsigned d)
{
    switch (d) {
    case 0: return "no battery";
    case 1: return "below pre-charge threshold";
    case 2: return "dead (timer)";
    case 3: return "OK";
    case 4: return "low voltage";
    case 5: return "overvoltage";
    default: return "?";
    }
}

static void fill_battery(void)
{
    struct ub u;
    uint16_t v = 0;
    uint8_t c = 0;
    if (bi_state < 0)
        bi_state = 0;                                  /* retry each time the page opens */
    if (!bi_init()) {
        kv_hdr("MAX77705");
        kv_addc("Bus", "no answer on HSI2C 0x14360000", UI_RED);
        kv_hex("TIMING_FS3 (S-Boot)", bi_fs3, 8);
        return;
    }
    if (fg_word(0x06, &v)) {
        ub_init(&u);
        ub_dec(&u, v >> 8);
        ub_s(&u, " %");
        kv_big("battery (RepSOC)", u.b);
    }
    kv_hdr("FUEL GAUGE (0x36)");
#define FG_ROW(reg, label, fmt)                                     \
    do {                                                            \
        ub_init(&u);                                                \
        if (fg_word(reg, &v)) {                                     \
            fmt;                                                    \
            kv_add(label, u.b);                                     \
        } else {                                                    \
            kv_addc(label, "read failed", UI_RED);                  \
        }                                                           \
    } while (0)
    FG_ROW(0x06, "State of charge (RepSOC)", ub_pct(&u, v));
    FG_ROW(0xff, "State of charge (VFSOC)", ub_pct(&u, v));
    FG_ROW(0x09, "Cell voltage", ub_mv(&u, v));
    FG_ROW(0x19, "Average cell voltage", ub_mv(&u, v));
    FG_ROW(0xfb, "Open-circuit voltage (VFOCV)", ub_mv(&u, v));
    FG_ROW(0x0a, "Current (+ = charging)", ub_ma(&u, v));
    FG_ROW(0x0b, "Average current", ub_ma(&u, v));
    FG_ROW(0x05, "Remaining capacity", (ub_dec(&u, v), ub_s(&u, " mAh")));
    FG_ROW(0x35, "Full capacity (learned)", (ub_dec(&u, v), ub_s(&u, " mAh")));
    FG_ROW(0x18, "Design capacity", (ub_dec(&u, v), ub_s(&u, v == 0x0b97 ? " mAh (model loaded)" : " mAh")));
    FG_ROW(0x17, "Cycles (raw)", ub_dec(&u, v));
    FG_ROW(0x08, "Temperature (last host write)", ub_t256(&u, (int16_t)v));
    FG_ROW(0x00, "Status", (ub_hex(&u, v, 4), ub_s(&u, (v & 8U) ? "  battery ABSENT" : "  battery present")));
#undef FG_ROW

    kv_hdr("CHARGER (0x69)");
    if (bi_read(BI_CHG, 0xb2, &c, 1)) {
        kv_addc("Input", (c & 0x20U) ? "wireless" : (c & 0x40U) ? "USB cable" : "none (on battery)",
                (c & 0x60U) ? UI_GREEN : UI_TEXT);
        kv_hex("CHG_INT_OK", c, 2);
    }
    if (bi_read(BI_CHG, 0xb3, &c, 1)) {
        unsigned d = (c >> 5) & 3U;
        kv_add("VBUS", d == 3 ? "valid" : d == 2 ? "over voltage" : d == 1 ? "low (below VBAT + margin)" : "none / UVLO");
    }
    if (bi_read(BI_CHG, 0xb4, &c, 1)) {
        unsigned d = c & 0xfU;
        kv_addc("Charger state", chg_dtls(d), d <= 2 ? UI_GREEN : d <= 4 ? UI_HOLO : d == 0xb ? UI_ORANGE : UI_TEXT);
        kv_add("Battery state", bat_dtls((c >> 4) & 7U));
    }
    if (bi_read(BI_CHG, 0xb9, &c, 1)) {
        ub_init(&u);
        ub_dec(&u, (c & 0x3fU) <= 2 ? 100U : (c & 0x3fU) * 50U);
        ub_s(&u, " mA");
        kv_add("Charge current limit", u.b);
    }
    if (bi_read(BI_CHG, 0xc0, &c, 1)) {
        unsigned r = c & 0x7fU;
        ub_init(&u);
        ub_dec(&u, r <= 3 ? 100U : r >= 0x7f ? 3200U : (r + 1U) * 25U);
        ub_s(&u, " mA");
        kv_add("Input current limit", u.b);
    }
    if (bi_read(BI_CHG, 0xb7, &c, 1)) {
        ub_init(&u);
        ub_hex(&u, c, 2);
        ub_s(&u, (c & 1U) ? "  charge" : "");
        ub_s(&u, (c & 4U) ? "  buck" : "");
        ub_s(&u, (c & 0x10U) ? "  watchdog on" : "");
        kv_add("CNFG_00 mode", u.b);
    }
    kv_hdr("CHIP");
    ub_init(&u);
    ub_s(&u, "MAX77705  id ");
    ub_hex(&u, bi_id, 2);
    ub_s(&u, "  rev ");
    ub_dec(&u, bi_rev & 7U);
    ub_s(&u, " ver ");
    ub_dec(&u, bi_rev >> 3);
    kv_add("PMIC", u.b);
    if (bi_read(BI_USBC, 0x08, &c, 1)) {
        unsigned t = c & 3U;
        kv_add("USB charger type", t == 1 ? "SDP (computer port)" : t == 2 ? "CDP" : t == 3 ? "DCP (wall charger)" : "none");
    }
    kv_addc("Note", "charger watchdog is not fed here: charging may pause", UI_TEXT3);
}

/* ---- TMU (temperatures), read only: build/em-research-thermal-clocks.md -- */

#define TMU_TOP 0x10070000UL
#define TMU_SUB 0x10074000UL

static const unsigned char big_probe[5] = { 1, 2, 3, 4, 6 };   /* DT mask 0x5E */

/* Probe p of a running TMU in degrees C; *flags |= 1 if uncalibrated. */
static int tmu_temp(uintptr_t base, unsigned p, unsigned *flags)
{
    uint32_t tri = rd(base + 4U * p);
    int te1 = (int)(tri & 0x1ffU), te2 = (int)((tri >> 9) & 0x1ffU);
    int two = (int)((rd(base) >> 23) & 1U);
    if (!te1) {
        te1 = 55;
        *flags |= 1U;
    }
    uintptr_t off = p < 2 ? 0x40U : 0x44U + 4U * ((p - 2U) / 3U);
    unsigned sh = p < 2 ? 9U * p : 9U * ((p - 2U) % 3U);
    int code = (int)((rd(base + off) >> sh) & 0x1ffU);
    return (two && te2 > te1) ? (code - te1) * 60 / (te2 - te1) + 25 : code - te1 + 25;
}

static uint32_t temp_col(int t)
{
    return t >= 85 ? UI_RED : t >= 65 ? UI_ORANGE : UI_TEXT;
}

static void kv_temp(const char *k, int t, unsigned flags)
{
    struct ub u;
    ub_init(&u);
    ub_sdec(&u, t);
    ub_s(&u, " " UI_DEG "C");
    if (flags & 1U)
        ub_s(&u, "  (uncalibrated)");
    kv_addc(k, u.b, temp_col(t));
}

/* 0 off, 1 running, 2 emulated */
static int tmu_state(uintptr_t base)
{
    if (!(rd(base + 0x20) & 1U))
        return 0;
    return (rd(base + 0x160) & 1U) ? 2 : 1;
}

static void fill_thermal(void)
{
    int top = tmu_state(TMU_TOP), sub = tmu_state(TMU_SUB), hot = -100;
    unsigned fl = 0;
    int big[5], little = 0, gpu = 0, isp = 0;
    if (top) {
        for (unsigned i = 0; i < 5; ++i) {
            big[i] = tmu_temp(TMU_TOP, big_probe[i], &fl);
            if (big[i] > hot)
                hot = big[i];
        }
        little = tmu_temp(TMU_TOP, 5, &fl);
        if (little > hot)
            hot = little;
    }
    if (sub) {
        gpu = tmu_temp(TMU_SUB, 1, &fl);
        isp = tmu_temp(TMU_SUB, 2, &fl);
        if (gpu > hot)
            hot = gpu;
        if (isp > hot)
            hot = isp;
    }
    if (hot > -100) {
        struct ub u;
        ub_init(&u);
        ub_sdec(&u, hot);
        ub_s(&u, UI_DEG "C");
        kv_put(KV_BIG, "hottest sensor", u.b, temp_col(hot));
    }
    kv_hdr("TMU_TOP 0x10070000");
    if (!top) {
        kv_addc("State", "off (not enabled by S-Boot)", UI_TEXT3);
    } else {
        if (top == 2)
            kv_addc("State", "EMULATION ON: values are fake", UI_ORANGE);
        int bmax = big[0];
        for (unsigned i = 1; i < 5; ++i)
            if (big[i] > bmax)
                bmax = big[i];
        kv_temp("Big cluster (Mongoose M3), max", bmax, fl);
        for (unsigned i = 0; i < 5; ++i) {
            struct ub k;
            ub_init(&k);
            ub_s(&k, "  M3 probe ");
            ub_dec(&k, big_probe[i]);
            kv_temp(k.b, big[i], 0);
        }
        kv_temp("Little cluster (Cortex-A55)", little, 0);
        kv_temp("Probe 0 (unassigned)", tmu_temp(TMU_TOP, 0, &fl), 0);
        kv_temp("Probe 7 (S-Boot log sensor)", tmu_temp(TMU_TOP, 7, &fl), 0);
        kv_add("Hardware trip", (rd(TMU_TOP + 0x20) & 0x1000U) ? "armed (115 " UI_DEG "C, S-Boot)" : "not armed");
    }
    kv_hdr("TMU_SUB 0x10074000");
    if (!sub) {
        kv_addc("State", "off (not enabled by S-Boot)", UI_TEXT3);
    } else {
        if (sub == 2)
            kv_addc("State", "EMULATION ON: values are fake", UI_ORANGE);
        kv_temp("GPU (Mali-G72)", gpu, 0);
        kv_temp("ISP (camera)", isp, 0);
        kv_temp("Probe 0 (unassigned)", tmu_temp(TMU_SUB, 0, &fl), 0);
        kv_add("Hardware trip", (rd(TMU_SUB + 0x20) & 0x1000U) ? "armed" : "not armed");
    }
    kv_hdr("CALIBRATION");
    kv_add("Mode", ((rd(TMU_TOP) >> 23) & 1U) ? "two-point (25/85 " UI_DEG "C eFuse)" : "one-point");
    kv_add("Source", "direct TMU read (APM reads the same)");
}

/* ---- clocks, read only ----------------------------------------------------- */

/* PLL_CON0: [31] enable, [29] locked, [25:16] M, [13:8] P, [4] mux (0 = 26 MHz), [2:0] S */
static uint64_t pll_hz(uintptr_t con0, int *unlocked)
{
    uint32_t v = rd(con0);
    if (!(v & 0x10U))
        return 26000000U;
    if (!(v & 0x80000000U))
        return 0;
    if (unlocked && !(v & 0x20000000U))
        *unlocked = 1;
    uint64_t m = (v >> 16) & 0x3ffU, p = (v >> 8) & 0x3fU, s = v & 7U;
    return p ? 26000000ULL * m / (p << s) : 0;
}

/* CPU cluster switch path through CMU_TOP */
static uint64_t top_switch(uintptr_t user, uintptr_t mux, uintptr_t div)
{
    uint64_t src;
    if (!((rd(user) >> 4) & 1U))
        return 26000000U;
    switch (rd(mux) & 3U) {
    case 0: src = pll_hz(0x1a240120UL, 0) / ((rd(0x1a2418d4UL) & 1U) + 1U); break;
    case 1: src = pll_hz(0x1a2401a0UL, 0) / ((rd(0x1a2418e0UL) & 1U) + 1U); break;
    case 2: src = pll_hz(0x1a240220UL, 0); break;
    default: src = pll_hz(0x1a240320UL, 0); break;
    }
    return src / ((rd(div) & 7U) + 1U);
}

static void kv_mhz(const char *k, uint64_t hz, const char *note)
{
    struct ub u;
    ub_init(&u);
    ub_fix(&u, hz / 1000U, 1);
    ub_s(&u, " MHz");
    if (note)
        ub_s(&u, note);
    kv_add(k, u.b);
}

/* ---- Mali-G72 (read only): the probe the iOS GPU driver starts from ---------
 * gpu@17500000 (job/mmu/gpu SPI 282/281/283), G3D domain PMU 0x14064040/44,
 * CMU_G3D 0x17400000, vdd_g3d = S2MPS18 BUCK6 (reg 0x2b, 300 mV + n x 6.25).
 * Registers are read only while the domain reports on: a powered-off block
 * stalls the bus (and EngineerMode's vectors catch a synchronous abort).
 */
#define MALI 0x17500000UL

static void kv_mali_hex(const char *k, uint32_t off)
{
    uint32_t v = 0;
    if (probe32(MALI + off, &v))
        kv_hex(k, v, 8);
    else
        kv_trap(k);
}

static void fill_gpu(void)
{
    struct ub u;
    uint32_t st = rd(0x14064044UL);
    uint8_t b6 = 0;
    kv_hdr("POWER (LIVE)");
    kv_hex("G3D domain status", st, 8);
    kv_addc("Domain", (st & 0xfU) == 0xfU ? "on (left on by S-Boot)" : "OFF", (st & 0xfU) == 0xfU ? UI_GREEN : UI_ORANGE);
    if (hw_pmic_read(1, 0x2b, &b6)) {
        ub_init(&u);
        ub_fix(&u, 300000U + b6 * 6250U, 3);
        ub_s(&u, " mV  (vsel ");
        ub_hex(&u, b6, 2);
        ub_s(&u, b6 >= 0x68 ? ")" : ", jobs need 950 mV)");
        kv_addc("BUCK6 vdd_g3d", u.b, b6 >= 0x68 ? UI_TEXT : UI_ORANGE);
    }
    if ((st & 0xfU) != 0xfU) {
        kv_addc("GPU registers", "not read: domain off", UI_TEXT3);
        return;
    }
    uint32_t con0 = rd(0x17400140UL);
    kv_hex("PLL_G3D CON0", con0, 8);
    kv_mhz("PLL_G3D", pll_hz(0x17400140UL, 0), (con0 & 0x20000000U) ? "  locked" : "  not stable");
    kv_hdr("MALI-G72 0x17500000 (LIVE)");
    uint32_t id = 0;
    if (probe32(MALI + 0x000, &id)) {
        ub_init(&u);
        ub_hex(&u, id, 8);
        ub_s(&u, (id >> 16) == 0x6221U ? "  Mali-G72" : "  unexpected");
        ub_s(&u, " r");
        ub_dec(&u, (id >> 12) & 0xfU);
        ub_c(&u, 'p');
        ub_dec(&u, (id >> 4) & 0xfU);
        kv_addc("GPU_ID", u.b, (id >> 16) == 0x6221U ? UI_GREEN : UI_ORANGE);
    } else {
        kv_trap("GPU_ID");
        return;
    }
    kv_mali_hex("L2_FEATURES", 0x004);
    kv_mali_hex("CORE_FEATURES", 0x008);
    kv_mali_hex("TILER_FEATURES", 0x00c);
    kv_mali_hex("MEM_FEATURES", 0x010);
    kv_mali_hex("MMU_FEATURES", 0x014);
    kv_mali_hex("AS_PRESENT", 0x018);
    kv_mali_hex("JS_PRESENT", 0x01c);
    kv_mali_hex("GPU_STATUS", 0x034);
    uint32_t sh = 0, rdy = 0;
    if (probe32(MALI + 0x100, &sh)) {
        unsigned n = 0;
        for (uint32_t x = sh; x; x &= x - 1)
            ++n;
        ub_init(&u);
        ub_hex(&u, sh, 8);
        ub_s(&u, "  ");
        ub_dec(&u, n);
        ub_s(&u, " shader cores");
        kv_add("SHADER_PRESENT", u.b);
    }
    kv_mali_hex("TILER_PRESENT", 0x110);
    kv_mali_hex("L2_PRESENT", 0x120);
    if (probe32(MALI + 0x140, &rdy))
        kv_addc("SHADER_READY", rdy ? "powered" : "off (powered on by a driver)", rdy ? UI_GREEN : UI_TEXT2);
    kv_mali_hex("L2_READY", 0x160);
    kv_hdr("FOR THE IOS DRIVER");
    kv_add("Interrupts", "job SPI 282, mmu 281, gpu 283");
    kv_add("Before jobs", "BUCK6 950 mV, PLL_G3D relock");
    kv_add("After a power cycle", "SMC 0x82000410 (TZPC restore)");
}

static void fill_clocks(void)
{
    int ul = 0;
    uint64_t a55src = (rd(0x1d00100cUL) & 1U) ? top_switch(0x1d000120UL, 0x1a241034UL, 0x1a241830UL)
                                              : pll_hz(0x1d000140UL, &ul);
    uint64_t a55 = a55src / ((rd(0x1d001818UL) & 0xfffU) + 1U);
    struct ub u;
    ub_init(&u);
    ub_dec(&u, a55 / 1000000U);
    ub_s(&u, " MHz");
    kv_big("Cortex-A55 (this core)", u.b);

    kv_hdr("CPU CLUSTERS");
    kv_mhz("A55 cluster", a55, (rd(0x1d00100cUL) & 1U) ? "  (switch path)" : ul ? "  (PLL unlocked)" : 0);
    if ((rd(0x14062044UL) & 0xfU) == 0xfU) {
        int ul1 = 0;
        uint64_t src = (rd(0x1d101000UL) & 1U) ? top_switch(0x1d100100UL, 0x1a241038UL, 0x1a241834UL)
                                               : pll_hz(0x1d100120UL, &ul1);
        kv_mhz("M3 cluster", src / ((rd(0x1d10180cUL) & 0xfffU) + 1U), ul1 ? "  (PLL unlocked)" : 0);
    } else {
        kv_addc("M3 cluster", "powered off", UI_TEXT3);
    }
    kv_hdr("GPU AND MEMORY");
    if ((rd(0x14064044UL) & 0xfU) == 0xfU)
        kv_mhz("GPU (PLL_G3D)", pll_hz(0x17400140UL, 0), (rd(0x17401000UL) & 1U) ? "  (bus on switch)" : 0);
    else
        kv_addc("GPU (PLL_G3D)", "powered off", UI_TEXT3);
    if (rd(0x1b801000UL) & 1U)
        kv_add("PLL_MIF (DDR PHY 2x)", "on switch path");
    else
        kv_mhz("PLL_MIF (DDR PHY 2x)", pll_hz(0x1b800120UL, 0), 0);
    kv_hdr("SHARED BUS PLLS (CMU_TOP)");
    kv_mhz("PLL_SHARED0", pll_hz(0x1a240120UL, 0), 0);
    kv_mhz("PLL_SHARED1", pll_hz(0x1a2401a0UL, 0), 0);
    kv_mhz("PLL_SHARED2", pll_hz(0x1a240220UL, 0), 0);
    kv_mhz("PLL_SHARED3", pll_hz(0x1a2402a0UL, 0), 0);
    kv_mhz("PLL_SHARED4", pll_hz(0x1a240320UL, 0), 0);
    kv_hdr("RAW PLL_CON0");
    kv_hex("PLL_CPUCL0", rd(0x1d000140UL), 8);
    kv_hex("PLL_MIF", rd(0x1b800120UL), 8);
    kv_hex("PLL_SHARED0", rd(0x1a240120UL), 8);
    kv_addc("Owner", "APM firmware (DVFS); read only here", UI_TEXT3);
}

/* ======================================================================= */
/* STORAGE                                                                    */
/* ======================================================================= */

static void fill_ufs(void)
{
    volatile uint32_t *h = (volatile uint32_t *)0x11120000UL;
    uint32_t cap = h[0x00 / 4], ver = h[0x08 / 4], hcs = h[0x30 / 4], is = h[0x20 / 4];
    struct ub u;
    kv_hdr("UFS HOST CONTROLLER 0x11120000 (LIVE)");
    kv_hex("CAP", cap, 8);
    ub_init(&u);
    ub_dec(&u, (cap & 0x1fU) + 1);
    ub_s(&u, " transfer slots, ");
    ub_dec(&u, ((cap >> 16) & 7U) + 1);
    ub_s(&u, " TM slots");
    kv_add("Slots", u.b);
    kv_add("64-bit DMA", (cap & (1U << 24)) ? "yes" : "no");
    ub_init(&u);
    ub_s(&u, "UFSHCI ");
    ub_dec(&u, (ver >> 8) & 0xfU);
    ub_c(&u, '.');
    ub_dec(&u, (ver >> 4) & 0xfU);
    kv_add("Version", u.b);
    kv_hex("HCS", hcs, 8);
    ub_init(&u);
    ub_s(&u, (hcs & 1U) ? "device present " : "no device ");
    ub_s(&u, (hcs & 2U) ? "UTRL ready" : "UTRL not ready");
    kv_add("Status", u.b);
    kv_hex("IS", is, 8);
    kv_hex("UTRLBA", ((uint64_t)h[0x54 / 4] << 32) | h[0x50 / 4], 9);
    kv_hex("UTRLDBR", h[0x58 / 4], 8);
    kv_hdr("EXYNOS VS_HCI 0x11121100");
    kv_hex("TXPRDT_ENTRY_SIZE", h[0x1100 / 4], 8);
    kv_hex("RXPRDT_ENTRY_SIZE", h[0x1104 / 4], 8);
    kv_hex("UTRL_NEXUS_TYPE", h[0x1140 / 4], 8);
    kv_hex("DATA_REORDER", h[0x1160 / 4], 8);
    kv_hex("AXIDMA_RWDATA_BURST", h[0x116c / 4], 8);
    kv_hdr("BOOT-TIME FACTS");
    kv_add("iOS driver", "multi-block, 16-byte PRDT, 64 KiB");
    kv_add("Windows driver", "S9 write fence (WINESP+WINDOWS)");
    kv_add("Device descriptor", "coming: needs a UFS driver here");
}


/* ======================================================================= */
/* OS                                                                         */
/* ======================================================================= */

static const char *rwd1_state(uint32_t s)
{
    switch (s) {
    case 0x00: return "empty";
    case 0x10: return "boot shim armed";
    case 0x20: return "SEC";
    case 0x30: return "DXE";
    case 0x40: return "BDS";
    case 0x50: return "ExitBootServices";
    case 0x60: return "Windows owned";
    case 0x70: return "P3 post-handoff";
    case 0x80: return "shutdown pending";
    case 0x90: return "controlled stop";
    case 0xa0: return "recovery pending";
    case 0xb0: return "TWRP ack";
    case 0xe0: return "fatal: watchdog not petted";
    default: return "?";
    }
}

static void fill_windows(void)
{
    uint64_t size = 0;
    struct ub u;
    uint64_t at = star_bundle(E.in, 0, &size);
    struct store_ent se;
    kv_hdr("FIRMWARE (UEFI IMAGE)");
    if (store_find("windows.uefi", &se)) {
        ub_init(&u);
        ub_size(&u, se.size);
        ub_s(&u, se.medium == STORE_SD ? "  OS store (microSD)" : "  OS store (UFS)");
        kv_add("windows.uefi", u.b);
    } else {
        kv_addc("windows.uefi", "not in the OS store", UI_RED);
    }
    if (at) {
        ub_init(&u);
        ub_size(&u, size);
        ub_s(&u, "  fallback");
        kv_add("BOOT bundle", u.b);
    }
    kv_add("Build", "src38: Starbite SEC/PEI + woa DXE");
    kv_add("RAM", "stage 3: 3.4 GB (BS-data banks)");
    kv_add("Clock", "PMIC RTC seed (patch 0023)");
    kv_add("Watchdog handshake", "RWD1 SEC_ACTIVE + P3 clear");
    kv_add("UFS driver", "9.0.0.1 + S9 write fence (re-signed)");
    kv_add("Edition", "Windows 11 Pro 23H2 ARM64");

    volatile uint32_t *r = (volatile uint32_t *)0xFED13D80UL;
    volatile uint32_t *p3 = (volatile uint32_t *)0xFED13E80UL;
    volatile uint32_t *w = (volatile uint32_t *)0xFED17F70UL;
    volatile uint32_t *nrm = (volatile uint32_t *)0xFED17F18UL;
    kv_hdr("LAST RUN (RETAINED RECORDS)");
    if (r[0] == 0x31445752U) {
        ub_init(&u);
        ub_s(&u, rwd1_state(r[4]));
        kv_add("RWD1 state", u.b);
        kv_dec("RWD1 owner", r[5], r[5] == 6 ? "  (Windows UFS)" : 0);
        kv_dec("RWD1 phase", r[6], r[6] == 10 ? "  (Windows running)" : 0);
        kv_dec("RWD1 reason", r[7], 0);
        kv_dec("Watchdog heartbeats", r[10], 0);
    } else {
        kv_addc("RWD1", "no record (cold reset clears it)", UI_TEXT3);
    }
    if (p3[0] == 0x31504d53U)
        kv_add("P3 (SMP1)", (p3[5] & 0xffU) == 0x70U ? "acknowledged by Windows" :
                            (p3[5] & 0xffU) == 0x60U ? "pre-branch (not acked)" : "other");
    else
        kv_addc("P3 (SMP1)", "no record", UI_TEXT3);
    if (w[0] == 0x31525757U) {
        ub_init(&u);
        ub_dec(&u, w[1]);
        ub_s(&u, " CPUs, ");
        ub_dec(&u, ((uint64_t)w[5] << 32 | w[4]) * 4096U >> 20);
        ub_s(&u, " MiB, ");
        ub_dec(&u, w[6]);
        ub_s(&u, " ranges");
        kv_add("Windows saw", u.b);
    } else {
        kv_addc("Windows witness", "no record", UI_TEXT3);
    }
    if (nrm[0] == 0x314d524eU)
        kv_add("UFS shutdown record", ((nrm[1] >> 16) == 2) ? "orderly stop" : "init only (no orderly stop)");
    kv_hex("PMU RST_STAT", *(volatile uint32_t *)(PMU_BASE + 0x404), 8);
}

static void fill_ios(void)
{
    struct ub u;
    kv_hdr("IOS 16.7.16 (20H392) " UI_DOT " IPHONE X USERLAND");
    kv_add("Kernel", "XNU 8796.141.3 STARLTE (herolte series)");
    if (E.in) {
        ub_init(&u);
        ub_hex(&u, E.in->phys_base, 9);
        ub_s(&u, " + ");
        ub_size(&u, E.in->mem_size);
        kv_add("RAM", u.b);
        kv_hex("Entry (VA)", E.in->entry_va, 16);
        kv_dec("Kernel segments", E.in->nsegs, 0);
        ub_init(&u);
        ub_size(&u, E.in->rd_end - E.in->rd_start);
        kv_add("Payload", u.b);
    }
    kv_add("Storage", "UFS multi-block (16 x 4 KiB)");
    kv_add("CPUs", "4x Cortex-A55");
    kv_hdr("BOOT-ARGS");
    for (unsigned p = 0, n = ui_strlen(E.cmd); p < n;) {
        char line[80];
        unsigned k = 0;
        while (p < n && k < 70)
            line[k++] = E.cmd[p++];
        line[k] = 0;
        kv_mono(line);
    }
}

static void build_ios_opts(void)
{
    struct ub u;
    ub_init(&u);
    ub_dec(&u, opt.cpus);
    ub_s(&u, opt.cpus == 1 ? " core" : " cores");
    row_add("CPU cores", u.b, 0);
    row_add("Root filesystem read-write", "ufsrw / rootrw", R_CHECK | (opt.rw ? R_ON : 0));
    row_add("Verbose boot", "-v: kernel log on screen", R_CHECK | (opt.verbose ? R_ON : 0));
    row_add("Touchscreen", "startouch", R_CHECK | (opt.touch ? R_ON : 0));
    row_add("Touch power-on (PMIC)", "startouchpwr", R_CHECK | (opt.touchpwr ? R_ON : 0));
    row_add("UFS multi-block I/O", "ufsmulti: 16 blocks per command", R_CHECK | (opt.multi ? R_ON : 0));
    row_add("CPU core probe", "starsmpprobe: experiment, parks cores", R_CHECK | R_WARN | (opt.smpprobe ? R_ON : 0));
    row_add("Boot iOS with these settings", "applies the boot-args above", 0);
}

static int act_ios_opts(int i)
{
    switch (i) {
    case 0: opt.cpus = opt.cpus == 4 ? 1 : opt.cpus == 1 ? 2 : 4; break;
    case 1: opt.rw = !opt.rw; break;
    case 2: opt.verbose = !opt.verbose; break;
    case 3: opt.touch = !opt.touch; break;
    case 4: opt.touchpwr = !opt.touchpwr; break;
    case 5: opt.multi = !opt.multi; break;
    case 6:
        if (opt.smpprobe || dialog("CPU core probe", "Parks CPU cores during boot.", "Reboot afterwards.", "ARM"))
            opt.smpprobe = !opt.smpprobe;
        break;
    default:
        if (!star_ios_pick())
            return 0;
        hw_opt_apply(E.cmd, E.len);
        E.boot = star_ios_pick();
        return 1;
    }
    return 0;
}

static void fs_kind(uint64_t lba, struct ub *u);   /* STORAGE section */

/* Store entries whose name starts with prefix, as rows "name  size  crc", both media. */
static void kv_store(const char *prefix)
{
    struct store_ent e;
    int shown = 0, any = 0;
    for (unsigned m = 0; m < STORE_MEDIA; ++m) {
        any |= store_open(m) > 0;
        for (unsigned i = 0; store_get(m, i, &e); ++i) {
            unsigned k = 0;
            while (prefix[k] && e.name[k] == prefix[k])
                ++k;
            if (prefix[k])
                continue;
            struct ub v;
            ub_init(&v);
            ub_size(&v, e.size);
            ub_s(&v, "  crc ");
            ub_hexn(&v, e.crc, 8);
            if (m == STORE_SD)
                ub_s(&v, "  SD");
            kv_add(e.name, v.b);
            ++shown;
        }
    }
    if (!shown)
        kv_addc("OS store", any ? "no entries for this OS" : "no store found", UI_RED);
}

/* Partition row: size and the filesystem found on it, live. */
static void kv_part(const char *label, const char *gpt, const char *role)
{
    uint64_t first, last;
    struct ub v;
    ub_init(&v);
    if (!gpt_find(gpt, &first, &last)) {
        kv_addc(label, "no such partition", UI_RED);
        return;
    }
    ub_size(&v, (last - first + 1) * 4096U);
    ub_s(&v, "  ");
    fs_kind(first, &v);
    if (role) {
        ub_s(&v, "  ");
        ub_s(&v, role);
    }
    kv_add(label, v.b);
}

static void fill_android(void)
{
    kv_hdr("ANDROID 17 " UI_DOT " LINEAGEOS 24.0");
    kv_add("Build", "lineage-24.0-20261025 unofficial (krazey90)");
    kv_add("Kernel", "Samsung Linux 4.9.337, first-stage init");
    kv_add("Device tree", "LineageOS DTBH in BOOT, S-Boot fixups");
    kv_add("Boot path", "loader: kernel + ramdisk from the OS store");
    kv_hdr("OS STORE (LIVE)");
    kv_store("android.");
    kv_hdr("PARTITIONS (LIVE)");
    kv_part("SYSTEM", "SYSTEM", "/system");
    kv_part("VENDOR", "VENDOR", "/vendor");
    kv_part("ODM", "ODM", "/metadata");
    kv_part("A16DATA", "A16DATA", "/data");
    kv_hdr("PROTECTIONS");
    kv_addc("/data", "A16DATA (vendor + ramdisk fstab, sepolicy)", UI_GREEN);
    kv_addc("TWRP /data", "A16DATA: recovery wipes cannot reach USERDATA", UI_GREEN);
    kv_addc("Updater", "removed: its OTA zips would write BOOT", UI_GREEN);
    kv_add("USERDATA", "never mounted by Android");
}

static void fill_linux(void)
{
    kv_hdr("LINUX 7.1 " UI_DOT " POSTMARKETOS (ALPINE EDGE)");
    kv_add("Kernel", "mainline 7.1 + Exynos 9810 port drivers");
    kv_add("Desktop", "Plasma Mobile, OpenRC");
    kv_add("Root", "microSD: /starbite-linux.img (ext4, 8 GiB)");
    kv_add("GPU", "Mali-G72 MC18 via Panfrost (Mesa)");
    kv_add("Display", "s9p-decon KMS, 60 Hz");
    kv_add("Wi-Fi", "BCM4361 via brcmfmac (PCIe)");
    kv_add("CPUs", "8 cores: 4x Cortex-A55 + 4x Mongoose M3");
    kv_add("USB", "network 172.16.42.1: SSH (telnet in rescue)");
    kv_add("Rescue", "hold Vol down while Linux starts");
    kv_add("Internal UFS", "read-only under Linux");
    kv_hdr("OS STORE (LIVE)");
    kv_store("linux.");
}

static void fill_store_medium(unsigned m)
{
    struct store_ent e;
    struct ub v;
    kv_hdr(m == STORE_UFS ? "UFS " UI_DOT " USERDATA +16 GIB (LIVE)" : "MICROSD " UI_DOT " PARTITION 2 (LIVE)");
    int n = store_open(m);
    if (!store_version(m)) {
        kv_addc("Store", "not found", UI_RED);
        return;
    }
    kv_add("Format", store_version(m) == 2 ? "SBSTORE2" : "SBSTORE1 (old)");
    kv_dec("First block", store_base(m), 0);
    kv_dec("Entries", (uint64_t)n, 0);
    ub_init(&v);
    ub_size(&v, store_used(m) * 4096U);
    ub_s(&v, " of ");
    ub_size(&v, store_capacity(m) * 4096U);
    kv_add("Used", v.b);
    for (unsigned i = 0; store_get(m, i, &e); ++i) {
        ub_init(&v);
        ub_size(&v, e.size);
        ub_s(&v, "  crc ");
        ub_hexn(&v, e.crc, 8);
        kv_add(e.name, v.b);
    }
}

static void fill_store(void)
{
    struct star_settings st;
    struct ub v;
    fill_store_medium(STORE_UFS);
    fill_store_medium(STORE_SD);
    kv_hdr("CATALOG");
    kv_add("Source", cat_builtin() ? "built in (no catalog in the store)" : "OS store");
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *c = cat_get(i);
        const char *why;
        ub_init(&v);
        ub_s(&v, c->key);
        ub_s(&v, "  ");
        ub_s(&v, star_entry_ready(i, &why) ? "ready" : why);
        kv_addc(c->name, v.b, star_entry_ready(i, 0) ? UI_TEXT : UI_RED);
    }
    kv_hdr("SETTINGS");
    if (!settings_load(&st)) {
        kv_addc("Record", "none (defaults)", UI_RED);
        return;
    }
    kv_add("Last choice", st.last_key[0] ? st.last_key : "-");
    kv_add("Default", st.default_key[0] ? st.default_key : "-");
    kv_dec("Timeout", st.timeout_s, " s (0 = wait)");
    kv_add("Next (one shot)", st.next_key[0] ? st.next_key : "-");
    kv_dec("Writes", st.seq, 0);
}

/* ---- microSD (sd.c) ---------------------------------------------------------
 * The card driver runs only from here until "Card store at boot" is on: a probe with the fault
 * vectors armed, every step shown. */
static void kv_ok(const char *k, int ok, const char *v)
{
    kv_addc(k, v, ok ? UI_GREEN : UI_RED);
}

static void fill_sd(void)
{
    static int probed;
    struct ub v;
    uint64_t faults = EMF[3];
    if (!probed) {
        sd_allow();
        (void)sd_up();
        store_reset(STORE_SD);
        (void)store_open(STORE_SD);
        probed = 1;
    }
    kv_hdr("CARD");
    kv_ok("Card detect (GPA1-5)", sd_rep.present, sd_rep.present ? "card present" : "no card");
    ub_init(&v);
    ub_s(&v, sd_rep.rails > 0 ? "on" : sd_rep.rails == 0 ? "refused" : "no ACPM");
    ub_s(&v, "  LDO31 ");
    ub_hexn(&v, sd_rep.l31, 2);
    ub_s(&v, "  LDO2 ");
    ub_hexn(&v, sd_rep.l2, 2);
    kv_ok("Power", sd_rep.rails > 0, v.b);
    kv_hdr("CONTROLLER (DWMMC2 0x11500000)");
    for (unsigned i = 0; i < 3; ++i) {
        ub_init(&v);
        ub_hexn(&v, sd_rep.gate[i], 8);
        kv_add(i == 0 ? "Gate CMU top 1a242070" : i == 1 ? "Gate ACLK 11402030" : "Gate SDCLKIN 11402034", v.b);
    }
    kv_hex("CMU divider 1a241864", sd_rep.cmu_div, 8);
    kv_hex("VERID", sd_rep.verid, 8);
    kv_hex("HCON", sd_rep.hcon, 8);
    kv_hex("CLKSEL64", sd_rep.clksel, 8);
    ub_init(&v);
    ub_dec(&v, sd_rep.bus_hz / 1000000U);
    ub_s(&v, " MHz bus, card ");
    ub_dec(&v, sd_rep.card_hz / 1000U);
    ub_s(&v, " kHz");
    kv_add("Clock", v.b);
    kv_hdr("SD INIT");
    kv_ok("Result", sd_rep.state > 0, sd_rep.state > 0 ? "ready, 4-bit" : sd_rep.why ? sd_rep.why : "not tried");
    kv_hex("CMD8 response", sd_rep.r7, 8);
    kv_hex("OCR", sd_rep.ocr, 8);
    kv_add("Capacity class", sd_rep.high_capacity ? "SDHC/SDXC (block addressed)" : "SDSC");
    kv_hex("RCA", sd_rep.rca, 4);
    ub_init(&v);
    uint32_t pnm[5] = { sd_rep.cid[3] & 255U, sd_rep.cid[2] >> 24, (sd_rep.cid[2] >> 16) & 255U,
                        (sd_rep.cid[2] >> 8) & 255U, sd_rep.cid[2] & 255U };
    for (unsigned i = 0; i < 5; ++i)
        ub_c(&v, pnm[i] >= 32 && pnm[i] < 127 ? (char)pnm[i] : '.');
    ub_s(&v, "  mfr ");
    ub_hexn(&v, sd_rep.cid[3] >> 24, 2);
    kv_add("CID product", v.b);
    kv_hex("Last RINTSTS", sd_rep.last_rintsts, 8);
    kv_hex("Last STATUS", sd_rep.last_status, 8);
    kv_dec("Faults while probing", EMF[3] - faults, 0);
    if (sd_rep.state <= 0)
        return;
    kv_hdr("MBR");
    static uint8_t mbr[512] __attribute__((aligned(8)));
    if (sd_read(0, 1, (uintptr_t)mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa) {
        kv_addc("Sector 0", "unreadable or no MBR", UI_RED);
    } else {
        for (unsigned i = 0; i < 4; ++i) {
            const uint8_t *e = mbr + 446 + 16 * i;
            uint32_t lba = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
            uint32_t len = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
            if (!e[4])
                continue;
            struct ub k;
            ub_init(&k);
            ub_s(&k, "p");
            ub_dec(&k, i + 1);
            ub_s(&k, " type ");
            ub_hexn(&k, e[4], 2);
            ub_init(&v);
            ub_dec(&v, lba);
            ub_s(&v, "  ");
            ub_size(&v, (uint64_t)len * 512U);
            kv_add(k.b, v.b);
        }
    }
    kv_hdr("OS STORE (PARTITION TYPE DA)");
    struct store_ent e;
    if (!store_version(STORE_SD)) {
        kv_addc("Store", "none on the card", UI_RED);
        return;
    }
    kv_dec("First block", store_base(STORE_SD), 0);
    ub_init(&v);
    ub_size(&v, store_used(STORE_SD) * 4096U);
    ub_s(&v, " of ");
    ub_size(&v, store_capacity(STORE_SD) * 4096U);
    kv_add("Used", v.b);
    for (unsigned i = 0; store_get(STORE_SD, i, &e); ++i) {
        ub_init(&v);
        ub_size(&v, e.size);
        ub_s(&v, "  crc ");
        ub_hexn(&v, e.crc, 8);
        kv_add(e.name, v.b);
    }
}

static void build_sdboot(void)
{
    struct star_settings st;
    int on = settings_load(&st) && (st.flags & SET_SD_STORE);
    row_add("Read the card's OS store at boot", "only after the microSD probe worked", R_CHECK | R_WARN | (on ? R_ON : 0));
}

static int act_sdboot(int i)
{
    struct star_settings st;
    (void)i;
    (void)settings_load(&st);
    if (!(st.flags & SET_SD_STORE) &&
        !dialog("Card store at boot", "The loader will power and read the card", "at every boot.", "TURN ON"))
        return 0;
    st.flags ^= SET_SD_STORE;
    if (settings_save(&st))
        (void)dialog("Card store at boot", "Could not write the settings record.", "Nothing changed.", "OK");
    return 0;
}

/* Start an OS straight from EngineerMode: every catalog entry (hidden ones too). E.boot = the
 * entry + 1, as the menu's pick. */
static void build_start(void)
{
    for (unsigned i = 0; i < cat_count(); ++i) {
        const struct cat_ent *c = cat_get(i);
        const char *why;
        int ok = star_entry_ready(i, &why);
        row_add(c->name, ok ? c->version : why, ok ? 0 : R_DIS);
    }
}

static int act_start(int i)
{
    if (i < 0 || (unsigned)i >= cat_count() || !star_entry_ready((unsigned)i, 0))
        return 0;
    if (!dialog("Start", rows[i].t, "Leaves EngineerMode.", "START"))
        return 0;
    if (i + 1 == star_ios_pick())
        hw_opt_apply(E.cmd, E.len);
    E.boot = i + 1;
    return 1;
}

/* ======================================================================= */
/* LOG AND DEBUGGING                                                          */
/* ======================================================================= */

static void show_pmsg(void)
{
    volatile uint32_t *h = (volatile uint32_t *)0xFED14000UL;
    const volatile char *d = (const volatile char *)0xFED1400CUL;
    const uint32_t cap = 0x3f00;
    tv_reset();
    if (h[0] != 0x43474244U) {
        tv_line("No pmsg ring at 0xFED14000.");
        text_screen("Boot log");
        return;
    }
    uint32_t start = h[1], size = h[2];
    if (size > cap)
        size = cap;
    uint32_t pos = size < cap ? 0 : start % cap;
    char line[200];
    unsigned k = 0;
    for (uint32_t i = 0; i < size; ++i) {
        char c = d[(pos + i) % cap];
        if (c == '\n' || k == sizeof(line) - 1) {
            line[k] = 0;
            tv_line(line);
            k = 0;
            if (c == '\n')
                continue;
        }
        line[k++] = (c >= 32 && c < 127) ? c : '.';
    }
    line[k] = 0;
    if (k)
        tv_line(line);
    text_screen("Boot log (pmsg)");
}

static void show_cmdline(void)
{
    tv_reset();
    tv_line("S-Boot /chosen bootargs:");
    tv_line("");
    if (!E.args) {
        tv_line("(none)");
    } else {
        char tok[200];
        unsigned k = 0;
        const volatile char *a = (const volatile char *)E.args;
        for (uint32_t i = 0; i <= E.args_len; ++i) {
            char c = i < E.args_len ? a[i] : ' ';
            if (c == ' ' || c == 0) {
                if (k) {
                    tok[k] = 0;
                    tv_line(tok);
                    k = 0;
                }
            } else if (k < sizeof(tok) - 1) {
                tok[k++] = c;
            }
        }
    }
    tv_line("");
    tv_line("iOS payload boot-args:");
    tv_line("");
    tv_line(E.cmd);
    text_screen("Kernel command lines");
}

static void hexrow(struct ub *u, uintptr_t a, unsigned words)
{
    ub_init(u);
    ub_hexn(u, a, 9);
    ub_s(u, ": ");
    for (unsigned i = 0; i < words; ++i) {
        ub_hexn(u, *(volatile uint32_t *)(a + 4 * i), 8);
        ub_c(u, ' ');
    }
}

static void show_records(void)
{
    struct ub u;
    tv_reset();
    tv_line("RWD1 watchdog record @0xFED13D80 (+cookie):");
    for (unsigned o = 0; o < 0x44; o += 16) {
        hexrow(&u, 0xFED13D80UL + o, o + 16 <= 0x44 ? 4 : 1);
        tv_line(u.b);
    }
    tv_line("");
    tv_line("P3 SMP1 record @0xFED13E80:");
    for (unsigned o = 0; o < 0x80; o += 16) {
        hexrow(&u, 0xFED13E80UL + o, 4);
        tv_line(u.b);
    }
    tv_line("");
    tv_line("Low WDT record @0xFED13800:");
    hexrow(&u, 0xFED13800UL, 3);
    tv_line(u.b);
    tv_line("");
    tv_line("Latch page @0xFED17F00 (NRM1 F18, WATL F40, witness F70, KEP FB0):");
    for (unsigned o = 0; o < 0x100; o += 16) {
        hexrow(&u, 0xFED17F00UL + o, 4);
        tv_line(u.b);
    }
    text_screen("Retained records");
}

static void fill_regs(void)
{
    volatile uint32_t *pmu = (volatile uint32_t *)PMU_BASE;
    kv_hdr("PMU 0x14060000 (LIVE)");
    kv_hex("SWRESET (+0x400)", pmu[0x400 / 4], 8);
    kv_hex("RST_STAT (+0x404)", pmu[0x404 / 4], 8);
    kv_hex("WDT_DISABLE (+0x408)", pmu[0x408 / 4], 8);
    kv_hex("WDT_MASK_RESET (+0x40C)", pmu[0x40c / 4], 8);
    kv_hex("INFORM2 (+0x808)", pmu[0x808 / 4], 8);
    kv_hex("INFORM3 (+0x80C)", pmu[0x80c / 4], 8);
    kv_hdr("WATCHDOG 0x10050000");
    kv_hex("WTCON", *(volatile uint32_t *)0x10050000UL, 8);
    kv_hex("WTCNT", *(volatile uint32_t *)0x10050008UL, 8);
    kv_hdr("CHIPID 0x10000000");
    kv_hex("PRO_ID", *(volatile uint32_t *)0x10000000UL, 8);
    kv_hdr("ALIVE GPIO 0x14050000");
    kv_hex("GPA0 DAT", *(volatile uint32_t *)0x14050024UL, 8);
    kv_hex("GPA1 DAT", *(volatile uint32_t *)0x14050044UL, 8);
    kv_hex("GPA2 DAT", *(volatile uint32_t *)0x14050064UL, 8);
    kv_hdr("UFS 0x11120000");
    kv_hex("HCS", *(volatile uint32_t *)0x11120030UL, 8);
    kv_hex("IS", *(volatile uint32_t *)0x11120020UL, 8);
}

/* ======================================================================= */
/* OTHERS                                                                     */
/* ======================================================================= */

/* ---- core bring-up test (PSCI 0.1 via SMC, starlte DTB /psci) ------------ */

#define BOX ((volatile uint64_t *)em_core_box)

static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                       "x16", "x17", "memory");
    return (int64_t)x0;
}

static struct core_result {
    int64_t ret;
    int slot;
    uint64_t mpidr, el, state;
    uint32_t ms;
} cores[7];
static int cores_done;

/* PSCI targets: A55 cpu1-3 = 1..3 (measured 2026-10-25), M3 = DTB reg 0x100..0x103. */
static uint64_t core_target(unsigned i)
{
    return i < 3 ? i + 1U : 0x100U + (i - 3U);
}

static const char *psci_err(int64_t r)
{
    switch (r) {
    case 0: return "SUCCESS";
    case -1: return "NOT_SUPPORTED";
    case -2: return "INVALID_PARAMETERS";
    case -3: return "DENIED";
    case -4: return "ALREADY_ON";
    case -5: return "ON_PENDING";
    case -6: return "INTERNAL_FAILURE";
    case -9: return "INVALID_ADDRESS";
    default: return "?";
    }
}

static void fill_cores(void)
{
    struct ub k, v;
    if (!cores_done) {
        kv_add("Status", "not run");
        return;
    }
    unsigned up = 0;
    for (unsigned i = 0; i < 7; ++i)
        up += cores[i].slot >= 0;
    ub_init(&v);
    ub_dec(&v, up + 1U);
    ub_s(&v, " / 8");
    kv_big("cores answered (cpu0 = this one)", v.b);
    kv_hdr("PER CORE: CPU_ON, CHECK-IN, CPU_OFF");
    kv_addc("cpu0 A55", "boot core, running EngineerMode", UI_GREEN);
    for (unsigned i = 0; i < 7; ++i) {
        const struct core_result *c = &cores[i];
        ub_init(&k);
        ub_s(&k, "cpu");
        ub_dec(&k, i + 1U);
        ub_s(&k, i < 3 ? " A55  PSCI " : " M3  PSCI ");
        ub_hex(&k, core_target(i), 3);
        ub_init(&v);
        if (c->ret) {
            ub_s(&v, "CPU_ON ");
            ub_s(&v, psci_err(c->ret));
            kv_addc(k.b, v.b, UI_RED);
        } else if (c->slot < 0) {
            ub_s(&v, "CPU_ON ok, no check-in in 1 s");
            kv_addc(k.b, v.b, UI_ORANGE);
        } else {
            ub_s(&v, "up in ");
            ub_dec(&v, c->ms);
            ub_s(&v, " ms, MPIDR ");
            ub_hex(&v, c->mpidr, 8);
            ub_s(&v, ", EL");
            ub_dec(&v, (c->el >> 2) & 3U);
            ub_s(&v, c->state == 2 ? ", PARKED" : ", off again");
            kv_addc(k.b, v.b, c->state == 2 ? UI_ORANGE : UI_GREEN);
        }
    }
    kv_hdr("NOTES");
    kv_add("Entry", "em_vec.S em_core_entry (MMU off)");
    kv_add("Parked core", "CPU_OFF returned: reboot before an OS");
}

static void run_cores(void)
{
    for (unsigned i = 0; i < 32; ++i)
        BOX[i] = 0;
    __asm__ volatile("dsb sy" : : : "memory");
    star_cache_inval(em_core_box, sizeof(em_core_box));   /* zeros to DRAM now, never later over a check-in */
    draw_status();
    draw_bar("Core bring-up test", 1);
    draw_nav();
    for (unsigned i = 0; i < 7; ++i) {
        struct core_result *c = &cores[i];
        uint64_t before[8];
        for (unsigned s = 0; s < 8; ++s)
            before[s] = BOX[s * 4];
        nkv = 0;
        kv_hdr("RUNNING");
        struct ub u;
        ub_init(&u);
        ub_s(&u, "CPU_ON ");
        ub_hex(&u, core_target(i), 3);
        kv_add("Starting", u.b);
        draw_kv(PAGE_Y0, NAV_Y, 0);
        c->slot = -1;
        c->state = 0;
        uint64_t t0 = hw_ticks();
        c->ret = psci_call(0xc4000003U, core_target(i), (uint64_t)(uintptr_t)em_core_entry, 0);
        if (c->ret)
            continue;
        for (unsigned ms = 0; ms < 1000 && c->slot < 0; ms += 1) {
            /* the core writes with its MMU off, straight to DRAM; ours caches .data (mmu.c) */
            star_cache_inval(em_core_box, sizeof(em_core_box));
            for (unsigned s = 0; s < 8; ++s)
                if (BOX[s * 4] != before[s]) {
                    c->slot = (int)s;
                    break;
                }
            if (c->slot < 0)
                hw_wait_ms(1);
        }
        c->ms = (uint32_t)((hw_ticks() - t0) * 1000U / hw_freq());
        if (c->slot >= 0) {
            hw_wait_ms(50);
            star_cache_inval(em_core_box, sizeof(em_core_box));
            c->mpidr = BOX[c->slot * 4 + 1];
            c->el = BOX[c->slot * 4 + 2];
            c->state = BOX[c->slot * 4];
        }
    }
    cores_done = 1;
    info_screen("Core bring-up test", fill_cores, 0);
    int parked = 0;
    for (unsigned i = 0; i < 7; ++i)
        parked |= cores[i].slot >= 0 && cores[i].state == 2;
    if (parked && !E.home && dialog("Core parked", "A core did not power off.", "Reboot before starting an OS.", "REBOOT"))
        hw_reboot(INFORM_NORMAL);
}

static void kv_ufs_fail(void)
{
    struct ub u;
    ub_init(&u);
    ub_s(&u, ufs_why == 1 ? "host not ready" : ufs_why == 2 ? "S-Boot request outstanding" :
             ufs_why == 3 ? "host refused our request list" : "device did not answer");
    kv_addc("UFS", u.b, UI_RED);
    ub_init(&u);
    ub_s(&u, "OCS ");
    ub_hex(&u, ufs_ocs, 2);
    ub_s(&u, "  UPIU ");
    ub_hex(&u, ufs_resp, 2);
    ub_s(&u, "  status ");
    ub_hex(&u, ufs_status, 2);
    kv_add("Last command", u.b);
}

/* UTF-16BE string descriptor to ASCII; hex if unprintable. */
static void ufs_string(uint8_t index, struct ub *u)
{
    uint8_t d[256];
    int n = ufs_desc(0x05, index, d, sizeof(d));
    if (n < 2) {
        ub_s(u, "?");
        return;
    }
    unsigned len = d[0] < (unsigned)n ? d[0] : (unsigned)n, printable = 1;
    for (unsigned i = 2; i + 1 < len; i += 2)
        if (d[i] || d[i + 1] < 32 || d[i + 1] > 126)
            printable = 0;
    for (unsigned i = 2; i + 1 < len && u->n < 60; i += 2) {
        if (printable)
            ub_c(u, (char)d[i + 1]);
        else
            ub_hexn(u, (uint64_t)d[i] << 8 | d[i + 1], 4);
    }
    while (printable && u->n && u->b[u->n - 1] == ' ')
        u->b[--u->n] = 0;
}

static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v = v << 8 | p[i];
    return v;
}

static const char *life_text(unsigned v)
{
    switch (v) {
    case 0x00: return "not reported";
    case 0x01: return "0-10 % used";
    case 0x02: return "10-20 % used";
    case 0x03: return "20-30 % used";
    case 0x04: return "30-40 % used";
    case 0x05: return "40-50 % used";
    case 0x06: return "50-60 % used";
    case 0x07: return "60-70 % used";
    case 0x08: return "70-80 % used";
    case 0x09: return "80-90 % used";
    case 0x0a: return "90-100 % used";
    case 0x0b: return "EXCEEDED rated life";
    default: return "?";
    }
}

static void fill_ufs_dev(void)
{
    struct ub u;
    uint8_t d[256];
    if (!ufs_up()) {
        kv_ufs_fail();
        return;
    }
    kv_hdr("SCSI INQUIRY (LU0)");
    static const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (!ufs_scsi(inq, 6, 36)) {
        char s[20];
        for (unsigned i = 0; i < 8; ++i)
            s[i] = (char)u8r(UFS_BUF + 8 + i);
        s[8] = 0;
        kv_add("Vendor", s);
        for (unsigned i = 0; i < 16; ++i)
            s[i] = (char)u8r(UFS_BUF + 16 + i);
        s[16] = 0;
        kv_add("Product", s);
        for (unsigned i = 0; i < 4; ++i)
            s[i] = (char)u8r(UFS_BUF + 32 + i);
        s[4] = 0;
        kv_add("Firmware revision", s);
    } else {
        kv_ufs_fail();
    }
    kv_hdr("DEVICE DESCRIPTOR");
    if (ufs_desc(0x00, 0, d, sizeof(d)) >= 0x2b) {
        ub_init(&u);
        ub_s(&u, "UFS ");
        ub_dec(&u, d[0x10]);
        ub_c(&u, '.');
        ub_dec(&u, d[0x11] >> 4);
        kv_add("Specification", u.b);
        uint16_t mid = (uint16_t)(d[0x18] << 8 | d[0x19]);
        ub_init(&u);
        ub_hex(&u, mid, 4);
        ub_s(&u, mid == 0x01ce ? "  Samsung" : mid == 0x0198 ? "  Toshiba" : mid == 0x01ad ? "  SK hynix" :
                 mid == 0x012c ? "  Micron" : "");
        kv_add("Manufacturer ID", u.b);
        ub_init(&u);
        ufs_string(d[0x15], &u);
        kv_add("Product name", u.b);
        ub_init(&u);
        ufs_string(d[0x2a], &u);
        kv_add("Product revision", u.b);
        ub_init(&u);
        ufs_string(d[0x16], &u);
        kv_add("Serial", u.b);
        kv_hex("Manufacture date", (uint64_t)d[0x12] << 8 | d[0x13], 4);
        kv_dec("Logical units", d[0x06], 0);
        kv_dec("Queue depth", d[0x21], 0);
        kv_hex("Device version", (uint64_t)d[0x22] << 8 | d[0x23], 4);
    } else {
        kv_ufs_fail();
    }
    kv_hdr("GEOMETRY");
    if (ufs_desc(0x07, 0, d, sizeof(d)) >= 12) {
        ub_init(&u);
        ub_size(&u, be64(d + 4) * 512U);
        kv_add("Raw capacity", u.b);
    }
    if (ufs_desc(0x02, 0, d, sizeof(d)) >= 0x13) {
        ub_init(&u);
        ub_size(&u, be64(d + 0x0b) << d[0x0a]);
        ub_s(&u, ", ");
        ub_dec(&u, 1U << d[0x0a]);
        ub_s(&u, "-byte blocks");
        kv_add("LU0", u.b);
    }
    kv_hdr("HEALTH (UFS 2.1 DESCRIPTOR 0x09)");
    if (ufs_desc(0x09, 0, d, sizeof(d)) >= 5) {
        unsigned eol = d[2];
        kv_addc("Reserved blocks (pre-EOL)", eol == 1 ? "normal" : eol == 2 ? "WARNING: 80 % consumed" :
                                             eol == 3 ? "URGENT: 90 % consumed" : "not reported",
                eol == 1 ? UI_GREEN : eol >= 2 ? UI_RED : UI_TEXT2);
        kv_addc("Wear, type A (SLC)", life_text(d[3]), d[3] >= 0x09 ? UI_ORANGE : UI_TEXT);
        kv_addc("Wear, type B (MLC/TLC)", life_text(d[4]), d[4] >= 0x09 ? UI_ORANGE : UI_TEXT);
    } else {
        kv_addc("Health", "descriptor not supported", UI_TEXT3);
    }
}

/* What sits at LU0 block lba (superblock magics), e.g. "ext4 \"vendor\"". */
static void fs_kind(uint64_t lba, struct ub *u)
{
    if (ufs_read(lba, 1)) {
        ub_s(u, "unreadable");
        return;
    }
    const uintptr_t b = UFS_BUF;
    uint32_t f2 = (uint32_t)u8r(b + 1024) | (uint32_t)u8r(b + 1025) << 8 | (uint32_t)u8r(b + 1026) << 16 |
                  (uint32_t)u8r(b + 1027) << 24;
    if (u8r(b + 1080) == 0x53 && u8r(b + 1081) == 0xef) {
        ub_s(u, "ext4");
        if (u8r(b + 1144)) {
            ub_s(u, " \"");
            for (unsigned i = 0; i < 16 && u8r(b + 1144 + i); ++i)
                ub_c(u, (char)u8r(b + 1144 + i));
            ub_c(u, '"');
        }
    } else if (f2 == 0xf2f52010U) {
        ub_s(u, "f2fs");
    } else if (u8r(b + 1024) == 'H' && (u8r(b + 1025) == '+' || u8r(b + 1025) == 'X')) {
        ub_s(u, "HFS+ (iOS)");
    } else if (u8r(b + 3) == 'N' && u8r(b + 4) == 'T' && u8r(b + 5) == 'F' && u8r(b + 6) == 'S') {
        ub_s(u, "NTFS");
    } else if (u8r(b + 82) == 'F' && u8r(b + 83) == 'A' && u8r(b + 84) == 'T') {
        ub_s(u, "FAT32");
    } else {
        int zero = 1;
        for (unsigned i = 0; i < 4096 && zero; i += 8)
            zero = !*(volatile uint64_t *)(b + i);
        ub_s(u, zero ? "empty" : "raw data");
    }
}

static void fill_partitions(void)
{
    struct ub k, v;
    if (gpt_load() < 0) {
        if (ufs_state < 0)
            kv_ufs_fail();
        else
            kv_addc("GPT", "not readable", UI_RED);
        return;
    }
    kv_hdr("LU0 GPT, LIVE (4 KIB BLOCKS)");
    uint64_t total = 0;
    for (int i = 0; i < gpt_n; ++i) {
        uint64_t n = gpt_last[i] - gpt_first[i] + 1;
        total += n;
        ub_init(&k);
        ub_s(&k, "sda");
        ub_dec(&k, (uint64_t)i + 1U);
        ub_s(&k, "  ");
        ub_s(&k, gpt_name[i]);
        ub_init(&v);
        ub_size(&v, n * 4096U);
        ub_s(&v, "  ");
        fs_kind(gpt_first[i], &v);
        const char *g = gpt_name[i];
        int mine = (g[0] == 'W' && g[1] == 'I' && g[2] == 'N') || (g[0] == 'A' && g[1] == '1' && g[2] == '6') ||
                   (g[0] == 'U' && g[1] == 'S' && g[2] == 'E' && g[3] == 'R') || (g[0] == 'B' && g[1] == 'O' && g[2] == 'O' && g[3] == 'T') ||
                   (g[0] == 'S' && g[1] == 'Y' && g[2] == 'S') || (g[0] == 'V' && g[1] == 'E' && g[2] == 'N') ||
                   (g[0] == 'O' && g[1] == 'D' && g[2] == 'M') || (g[0] == 'R' && g[1] == 'E' && g[2] == 'C');
        kv_addc(k.b, v.b, mine ? UI_HOLO : UI_TEXT);
    }
    ub_init(&v);
    ub_size(&v, total * 4096U);
    kv_add("Partitioned", v.b);
    kv_hdr("STARBITE");
    kv_add("BOOT", "this loader + iOS payload + Windows UEFI");
    kv_add("RECOVERY", "TWRP 3.7.0 (/data = A16DATA)");
    kv_add("USERDATA", "iOS root, OS store (+16 GiB), free (+17 GiB)");
    kv_add("SYSTEM / VENDOR", "Android 17 (LineageOS 24.0)");
    kv_add("ODM", "Android /metadata");
    kv_add("A16DATA", "Android /data (f2fs)");
    kv_add("WINESP / WINDOWS", "Windows 11");
    kv_add("microSD", "postmarketOS root (image file)");
}

/* ---- speed test --------------------------------------------------------------- */

static uint64_t sp_single, sp_multi;
static int sp_done, sp_multi_ok;
static uint64_t sp_lba;

static void fill_speed(void)
{
    struct ub u;
    if (!sp_done) {
        kv_ufs_fail();
        return;
    }
    ub_init(&u);
    ub_dec(&u, sp_multi_ok ? sp_multi : sp_single);
    ub_s(&u, " MB/s");
    kv_big(sp_multi_ok ? "sequential read, 64 KiB commands" : "sequential read, 4 KiB commands", u.b);
    kv_hdr("16 MIB FROM SYSTEM, READ ONLY");
    ub_init(&u);
    ub_dec(&u, sp_single);
    ub_s(&u, " MB/s");
    kv_add("4 KiB per command (S-Boot host state)", u.b);
    ub_init(&u);
    if (sp_multi_ok) {
        ub_dec(&u, sp_multi);
        ub_s(&u, " MB/s");
    } else {
        ub_s(&u, "self-test failed: skipped");
    }
    kv_addc("64 KiB per command (16 x 16-byte PRDT)", u.b, sp_multi_ok ? UI_TEXT : UI_ORANGE);
    kv_dec("Start block", sp_lba, 0);
    kv_hdr("NOTE");
    kv_add("Polled, one command at a time", "iOS uses 64 KiB commands");
}

static int bufs_equal(uintptr_t a, uintptr_t b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 8)
        if (*(volatile uint64_t *)(a + i) != *(volatile uint64_t *)(b + i))
            return 0;
    return 1;
}

static void run_speed(void)
{
    const uint32_t total = 4096;                    /* 16 MiB in 4 KiB blocks */
    sp_done = 0;
    sp_multi_ok = 0;
    if (!ufs_up()) {
        info_screen("Speed test", fill_speed, 0);
        return;
    }
    sp_lba = 68736;                                 /* SYSTEM (ext4), any readable range */
    if (gpt_load() > 0)
        for (int i = 0; i < gpt_n; ++i)
            if (gpt_name[i][0] == 'S' && gpt_name[i][1] == 'Y' && gpt_name[i][2] == 'S')
                sp_lba = gpt_first[i];
    draw_status();
    draw_bar("Speed test", 1);
    draw_nav();
    nkv = 0;
    kv_hdr("RUNNING");
    kv_add("Pass", "4 KiB commands, then 64 KiB");
    draw_kv(PAGE_Y0, NAV_Y, 0);
    uint64_t t0 = hw_ticks();
    for (uint32_t i = 0; i < total; ++i)
        if (ufs_read(sp_lba + i, 1)) {
            info_screen("Speed test", fill_speed, 0);
            return;
        }
    uint64_t dt = hw_ticks() - t0;
    sp_single = dt ? (uint64_t)total * 4096U * hw_freq() / dt / 1000000U : 0;
    sp_done = 1;

    /* 16-block commands as iOS does (starlte_ufs_pick_stride): verify first. */
    uint32_t tx0 = rd(UFS + 0x1100), rx0 = rd(UFS + 0x1104), ro0 = rd(UFS + 0x1160), ax0 = rd(UFS + 0x116c);
    const uintptr_t ref = UFS_BUF + (512U << 10);
    int ok = !ufs_read(sp_lba, 16);
    for (uint32_t i = 0; ok && i < 16U * 4096U; i += 8)
        *(volatile uint64_t *)(ref + i) = *(volatile uint64_t *)(UFS_BUF + i);
    wr(UFS + 0x1100, 0x8000000cU);
    wr(UFS + 0x1104, 0x8000000cU);
    wr(UFS + 0x1160, 0xaU);
    wr(UFS + 0x116c, 0x98000003U);
    if (ok) {
        u_zero(UFS_BUF, 16U * 4096U);
        ok = !ufs_read(sp_lba, 16) && bufs_equal(UFS_BUF, ref, 16U * 4096U);
    }
    if (ok) {
        t0 = hw_ticks();
        for (uint32_t i = 0; ok && i < total; i += 16)
            ok = !ufs_read(sp_lba + i, 16);
        dt = hw_ticks() - t0;
        sp_multi = dt ? (uint64_t)total * 4096U * hw_freq() / dt / 1000000U : 0;
        sp_multi_ok = ok;
    }
    wr(UFS + 0x1100, tx0);
    wr(UFS + 0x1104, rx0);
    wr(UFS + 0x1160, ro0);
    wr(UFS + 0x116c, ax0);
    info_screen("Speed test", fill_speed, 0);
}

/* ---- sector viewer -------------------------------------------------------------- */

static uint64_t sv_lba = 1;
static int sv_err;

static void sv_load(void)
{
    struct ub u;
    tv_reset();
    sv_err = !ufs_up() || ufs_read(sv_lba, 1);
    if (sv_err) {
        ub_init(&u);
        ub_s(&u, "Read failed: OCS ");
        ub_hex(&u, ufs_ocs, 2);
        ub_s(&u, " UPIU ");
        ub_hex(&u, ufs_resp, 2);
        ub_s(&u, " status ");
        ub_hex(&u, ufs_status, 2);
        tv_line(u.b);
        tv_line("(protected range? S-Boot's UFSP regions stay as they are)");
        return;
    }
    for (unsigned o = 0; o < 4096; o += 16) {
        ub_init(&u);
        ub_hexn(&u, o, 3);
        ub_s(&u, "  ");
        for (unsigned i = 0; i < 16; ++i) {
            ub_hexn(&u, u8r(UFS_BUF + o + i), 2);
            ub_c(&u, i == 7 ? '-' : ' ');
        }
        ub_c(&u, ' ');
        for (unsigned i = 0; i < 16; ++i) {
            uint8_t c = u8r(UFS_BUF + o + i);
            ub_c(&u, c >= 32 && c < 127 ? (char)c : '.');
        }
        tv_line(u.b);
    }
}

#define SV_BTN_Y PAGE_Y0
#define SV_BTN_H 150

static void sv_head(void)
{
    struct ub u;
    ui_clip(0, UI_H);
    ui_fill(0, SV_BTN_Y, UI_W, SV_BTN_H, UI_BG);
    static const int bx[4] = { 0, 300, 840, 1140 };
    for (int b = 0; b < 4; ++b) {
        int w = b == 1 || b == 2 ? 296 : 296;
        ui_fill(bx[b] + 4, SV_BTN_Y + 8, w, SV_BTN_H - 16, UI_BAR);
        ui_text_c(F_TAB, bx[b] + 4 + w / 2, SV_BTN_Y + 92, b == 0 ? UI_LEFT " -1" : b == 1 ? "-256" : b == 2 ? "+256" : "+1 " UI_RIGHT,
                  UI_HOLO, UI_BAR);
    }
    ub_init(&u);
    ub_dec(&u, sv_lba);
    ui_text_c(F_TITLE, UI_W / 2, SV_BTN_Y + 82, u.b, sv_err ? UI_RED : UI_TEXT, UI_BG);
    ub_init(&u);
    ub_s(&u, "block ");
    const char *part = "";
    for (int i = 0; i < gpt_n; ++i)
        if (sv_lba >= gpt_first[i] && sv_lba <= gpt_last[i])
            part = gpt_name[i];
    ub_s(&u, part[0] ? part : "LU0");
    ui_text_c(F_SMALL, UI_W / 2, SV_BTN_Y + 132, u.b, UI_TEXT2, UI_BG);
}

static void build_sv_parts(void)
{
    row_add("GPT header", "block 1", 0);
    for (int i = 0; i < gpt_n && nrows < MAX_ROWS; ++i) {
        struct ub u;
        ub_init(&u);
        ub_s(&u, "block ");
        ub_dec(&u, gpt_first[i]);
        row_add(gpt_name[i], u.b, 0);
    }
}

static int act_sv_parts(int i)
{
    sv_lba = i == 0 ? 1 : gpt_first[i - 1];
    return 1;
}

static void sector_screen(void)
{
    const int lh = 46, y0 = SV_BTN_Y + SV_BTN_H, view = NAV_Y - y0;
    int scroll = 0;
    (void)gpt_load();
    sv_load();
    for (int redraw = 1;;) {
        if (redraw) {
            draw_status();
            draw_bar("Sector viewer", 1);
            draw_nav();
            sv_head();
            draw_tv(y0, NAV_Y, scroll);
            redraw = 0;
        }
        struct ev e;
        int old = scroll, move = 0;
        ev_wait(&e, 0);
        if (e.type == EV_BACK)
            return;
        if (e.type == EV_HOME) {
            E.home = 1;
            return;
        }
        if (e.type == EV_SELECT) {
            move = 1;
        } else if (e.type == EV_TAP) {
            int c = chrome_tap(&e, 1);
            if (c == 1 || c == 3)
                return;
            if (c == 2) {
                E.home = 1;
                return;
            }
            if (e.y >= SV_BTN_Y && e.y < y0) {
                if (e.x < 300)
                    move = -1;
                else if (e.x < 600)
                    move = -256;
                else if (e.x >= 1140)
                    move = 1;
                else if (e.x >= 840)
                    move = 256;
                else if (gpt_n > 0) {
                    list_screen("Go to partition", build_sv_parts, act_sv_parts);
                    if (E.home)
                        return;
                    sv_load();
                    scroll = 0;
                    redraw = 1;
                }
            }
        } else if (e.type == EV_DOWN) {
            scroll += view - lh * 2;
        } else if (e.type == EV_UP) {
            scroll -= view - lh * 2;
        } else if (e.type == EV_DRAG) {
            scroll -= e.d;
        }
        if (move) {
            if (move < 0 && (uint64_t)-move > sv_lba)
                sv_lba = 0;
            else
                sv_lba = (uint64_t)((int64_t)sv_lba + move);
            sv_load();
            scroll = 0;
            redraw = 1;
            continue;
        }
        scroll = clamp_scroll(scroll, tv_n * lh + lh, view);
        if (scroll != old)
            draw_tv(y0, NAV_Y, scroll);
    }
}

/* ---- panel brightness (S6E3HA8 AOR, as S-Boot does it) ----------------------
 * build/em-research-brightness.md. S-Boot dims this panel with AOR only:
 * F0 5A 5A (level-2 key), B1 hi lo (AOR), F7 03 (commit), F0 A5 A5, all DCS
 * long writes (0x39) through DSIM 0x16080000: payload FIFO first, then the
 * header, then wait for the header FIFO to drain. Never sends gamma, ELVSS,
 * voltages, POC/MTP writes, sleep or display off. The next OS re-inits the
 * panel, so the setting lasts until then.
 */
#define DSIM 0x16080000UL
#define DSIM_INTSRC (DSIM + 0x50)
#define DSIM_PKTHDR (DSIM + 0x58)
#define DSIM_PAYLOAD (DSIM + 0x5c)
#define DSIM_RXFIFO (DSIM + 0x60)
#define DSIM_FIFOCTRL (DSIM + 0x68)
#define DSIM_PH_EMPTY (1U << 28)
#define DSIM_RX_DONE (1U << 18)

static uint32_t panel_aor = 0x0440;     /* S-Boot's normal level, assumed */

static int dsim_ready(void)
{
    return (rd(DSIM + 0x20) & (1U << 20)) && !(rd(DSIM + 0x4c) & (1U << 18));
}

static void dsim_hdr(uint32_t id, uint32_t d0, uint32_t d1, uint32_t bta)
{
    uint32_t v = rd(DSIM_PKTHDR) & 0xfe000000U;
    wr(DSIM_PKTHDR, v | (bta << 24) | ((d1 & 0xffU) << 16) | ((d0 & 0xffU) << 8) | id);
}

static int dsim_wait(void)
{
    for (unsigned t = 0; t < 50000; t += 10) {
        uint32_t s = rd(DSIM_INTSRC), f = rd(DSIM_FIFOCTRL);
        if ((s & DSIM_PH_EMPTY) || ((f & (1U << 10)) && !((f >> 16) & 0x3fU))) {
            wr(DSIM_INTSRC, DSIM_PH_EMPTY);
            return 0;
        }
        hw_wait_us(10);
    }
    return -1;
}

static int dcs_long(const uint8_t *b, uint32_t n)
{
    if (((rd(DSIM_FIFOCTRL) >> 16) & 0x3fU) && dsim_wait())
        return -1;
    wr(DSIM_INTSRC, DSIM_PH_EMPTY);
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (uint32_t k = 0; k < 4 && i + k < n; ++k)
            w |= (uint32_t)b[i + k] << (8 * k);
        wr(DSIM_PAYLOAD, w);
    }
    dsim_hdr(0x39, n & 0xffU, n >> 8, 0);
    return dsim_wait();
}

static int panel_set_aor(uint32_t aor)
{
    if (!dsim_ready())
        return -1;
    if (aor < 0x000c)
        aor = 0x000c;
    if (aor > 0x0ba3)
        aor = 0x0ba3;
    const uint8_t key_on[3] = { 0xf0, 0x5a, 0x5a };
    const uint8_t commit[2] = { 0xf7, 0x03 };
    const uint8_t key_off[3] = { 0xf0, 0xa5, 0xa5 };
    const uint8_t b1[3] = { 0xb1, (uint8_t)(aor >> 8), (uint8_t)aor };
    int r = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        r = dcs_long(key_on, 3);
        if (!r)
            r = dcs_long(b1, 3);
        if (!r)
            r = dcs_long(commit, 2);
        (void)dcs_long(key_off, 3);             /* always lock again */
        if (!r)
            break;
        hw_wait_ms(20);
    }
    if (!r)
        panel_aor = aor;
    return r;
}

/* DCS read 0x04 (panel ID, 3 bytes; S-Boot read 91 04 44): bytes, or < 0. */
static int panel_read_id(uint8_t out[3])
{
    if (!dsim_ready())
        return -1;
    for (int g = 0; g < 64 && !(rd(DSIM_FIFOCTRL) & (1U << 12)); ++g)
        (void)rd(DSIM_RXFIFO);
    wr(DSIM_INTSRC, DSIM_RX_DONE | DSIM_PH_EMPTY);
    dsim_hdr(0x37, 3, 0, 0);
    if (dsim_wait())
        return -1;
    wr(DSIM_INTSRC, DSIM_RX_DONE);
    dsim_hdr(0x06, 0x04, 0, 1);
    for (unsigned t = 0; !(rd(DSIM_INTSRC) & DSIM_RX_DONE); t += 10) {
        if (t > 100000)
            return -1;
        hw_wait_us(10);
    }
    wr(DSIM_INTSRC, DSIM_RX_DONE | DSIM_PH_EMPTY);
    for (int g = 0; g < 64 && !(rd(DSIM_FIFOCTRL) & (1U << 12)); ++g) {
        uint32_t w = rd(DSIM_RXFIFO);
        switch (w & 0xffU) {
        case 0x1c: {
            uint32_t wc = (w >> 8) & 0xffffU, d = rd(DSIM_RXFIFO);
            out[0] = (uint8_t)d;
            out[1] = (uint8_t)(d >> 8);
            out[2] = (uint8_t)(d >> 16);
            for (uint32_t i = 4; i < wc; i += 4)
                (void)rd(DSIM_RXFIFO);
            return 3;
        }
        case 0x21:
            out[0] = (uint8_t)(w >> 8);
            return 1;
        case 0x22:
            out[0] = (uint8_t)(w >> 8);
            out[1] = (uint8_t)(w >> 16);
            return 2;
        case 0x02:
            return -2;
        default:
            break;
        }
    }
    return -1;
}

static uint32_t bright_aor(int i)
{
    switch (i) {
    case 0: return 0x000c;
    case 1: return 0x0200;
    case 2: return 0x0440;
    case 3: return 0x0800;
    case 4: return 0x0b7c;
    default: return 0x0ba3;
    }
}

static int bright_msg;          /* 0 none, 1 ok, -1 failed, 2 id read */
static uint8_t bright_id[3];
static int bright_idn;

static void build_bright(void)
{
    struct ub u;
    int ok = dsim_ready();
    for (int i = 0; i < 6; ++i) {
        ub_init(&u);
        ub_s(&u, "AOR ");
        ub_hex(&u, bright_aor(i), 4);
        if (i == 2)
            ub_s(&u, "  (S-Boot normal)");
        row_add(i == 0 ? "Maximum" : i == 1 ? "Bright (~83 %)" : i == 2 ? "Normal (~64 %)" : i == 3 ? "Dim (~31 %)" :
                i == 4 ? "Very dim (~4 %)" : "Darkest (~2 %)",
                u.b, (ok ? 0 : R_DIS) | R_CHECK | (panel_aor == bright_aor(i) ? R_ON : 0));
    }
    ub_init(&u);
    if (bright_idn > 0) {
        ub_s(&u, "read: ");
        for (int i = 0; i < bright_idn; ++i) {
            ub_hexn(&u, bright_id[i], 2);
            ub_c(&u, ' ');
        }
        ub_s(&u, bright_idn == 3 && bright_id[0] == 0x91 ? "(S6E3HA8)" : "");
    } else {
        ub_s(&u, bright_idn < 0 ? "read failed" : "DCS read 0x04 (S-Boot read 91 04 44)");
    }
    row_add("Read panel ID", u.b, ok ? 0 : R_DIS);
    row_add(ok ? (bright_msg < 0 ? "Last write: no answer from DSIM" : "Lasts until an OS starts") : "DSI link not in command mode",
            "AOR only, like S-Boot: no gamma or voltage writes", R_DIS);
}

static int act_bright(int i)
{
    if (i < 6) {
        bright_msg = panel_set_aor(bright_aor(i)) ? -1 : 1;
    } else if (i == 6) {
        bright_idn = panel_read_id(bright_id);
        if (bright_idn == 0)
            bright_idn = -1;
    }
    return 0;
}

static void power_off(void)
{
    ui_clip(0, UI_H);
    ui_fill(0, 0, UI_W, UI_H, UI_BLACK);
    ui_text_c(F_TITLE, UI_W / 2, 1440, "Powering off" UI_ELLIPSIS, UI_HOLO, UI_BLACK);
    ui_text_c(F_SMALL, UI_W / 2, 1540, "release all keys", UI_TEXT2, UI_BLACK);
    hw_power_off();
    /* Still here: a cable keeps the PMIC on (Samsung reboots to LP charging). */
    ui_fill(0, 1380, UI_W, 200, UI_BLACK);
    ui_text_c(F_BODY, UI_W / 2, 1440, "Still powered (charger connected?)", UI_ORANGE, UI_BLACK);
    ui_text_c(F_SMALL, UI_W / 2, 1520, "rebooting instead", UI_TEXT2, UI_BLACK);
    hw_wait_ms(1500);
    hw_reboot(INFORM_NORMAL);
}

static void build_reboot(void)
{
    row_add("Reboot", "normal boot: S-Boot, this menu", 0);
    row_add("Reboot to recovery", "TWRP 3.7.0", 0);
    row_add("Reboot to download", "Odin mode", R_WARN);
    row_add("Power off", "PS_HOLD low (with a cable: reboots)", 0);
    row_add("Back to the boot menu", "leave EngineerMode", 0);
}

static int act_reboot(int i)
{
    if (i == 4) {
        E.home = 1;
        return 1;
    }
    if (i == 3) {
        if (dialog("Power off", "Turn the phone off now?", 0, "POWER OFF"))
            power_off();
        return 0;
    }
    const char *what = i == 0 ? "Reboot now?" : i == 1 ? "Reboot to TWRP?" : "Reboot to Odin mode?";
    if (dialog("Reboot", what, 0, "REBOOT")) {
        ui_clip(0, UI_H);
        ui_fill(0, 0, UI_W, UI_H, UI_BLACK);
        ui_text_c(F_TITLE, UI_W / 2, 1480, "Rebooting" UI_ELLIPSIS, UI_HOLO, UI_BLACK);
        hw_tp_clear();
        hw_reboot(i == 0 ? INFORM_NORMAL : i == 1 ? INFORM_RECOVERY : INFORM_DOWNLOAD);
    }
    return 0;
}

/* ---- memory bandwidth ------------------------------------------------------- */

static uint64_t bw_rd, bw_wr, bw_cp;
static int bw_done;

static uint64_t mib_s(uint64_t bytes, uint64_t ticks)
{
    return ticks ? bytes * hw_freq() / ticks / (1U << 20) : 0;
}

static void fill_bw(void)
{
    struct ub u;
    kv_hdr("16 MIB AT 0x092000000, 64-BIT ACCESSES");
    if (!bw_done) {
        kv_add("Status", "not run");
        return;
    }
    ub_init(&u);
    ub_dec(&u, bw_rd);
    ub_s(&u, " MiB/s");
    kv_big("read", u.b);
    ub_init(&u);
    ub_dec(&u, bw_wr);
    ub_s(&u, " MiB/s");
    kv_add("Write", u.b);
    ub_init(&u);
    ub_dec(&u, bw_cp);
    ub_s(&u, " MiB/s");
    kv_add("Copy (8 MiB)", u.b);
    kv_hdr("NOTE");
    kv_add("Caches", "off: the loader runs with the MMU off,");
    kv_add("", "so every access goes to DRAM (device memory).");
    kv_add("", "iOS, Windows and Linux run 10-50x faster.");
}

static void run_bw(void)
{
    volatile uint64_t *m = (volatile uint64_t *)RAMTEST_BASE;
    const uint64_t n = RAMTEST_SIZE / 8;
    uint64_t sum = 0, t;
    draw_status();
    draw_bar("Memory bandwidth", 1);
    draw_nav();
    nkv = 0;
    kv_hdr("RUNNING");
    kv_add("Pass", "write, read, copy");
    draw_kv(PAGE_Y0, NAV_Y, 0);
    t = hw_ticks();
    for (uint64_t i = 0; i < n; ++i)
        m[i] = i;
    bw_wr = mib_s(RAMTEST_SIZE, hw_ticks() - t);
    t = hw_ticks();
    for (uint64_t i = 0; i < n; ++i)
        sum += m[i];
    bw_rd = mib_s(RAMTEST_SIZE, hw_ticks() - t);
    t = hw_ticks();
    for (uint64_t i = 0; i < n / 2; ++i)
        m[n / 2 + i] = m[i];
    bw_cp = mib_s(RAMTEST_SIZE / 2, hw_ticks() - t);
    (void)sum;
    bw_done = 1;
    info_screen("Memory bandwidth", fill_bw, 0);
}

/* ---- display panel ----------------------------------------------------------- */

static void fill_panel(void)
{
    char v[64];
    struct ub u;
    kv_hdr("PANEL");
    kv_add("Panel", "Samsung S6E3HA8 AMOLED, 5.8\"");
    kv_add("Resolution", "1440 " UI_TIMES " 2960 (18.5:9), 570 ppi");
    if (bootarg("lcdtype", v, sizeof(v))) {
        uint64_t id = 0;
        for (unsigned i = 0; v[i]; ++i)
            id = id * 10U + (uint64_t)(v[i] >= '0' && v[i] <= '9' ? v[i] - '0' : 0);
        ub_init(&u);
        ub_s(&u, v);
        ub_s(&u, "  = ID ");
        ub_hexn(&u, id, 6);
        kv_add("lcdtype (S-Boot)", u.b);
    }
    kv_add("Interface", "MIPI DSI, command mode (DECON_F)");
    kv_hdr("FRAMEBUFFER (S-BOOT)");
    kv_hex("Base", STAR_FB_BASE, 9);
    kv_add("Format", "32 bpp BGRA, 5760 bytes per line");
    ub_init(&u);
    ub_size(&u, (uint64_t)STAR_FB_WIDTH * STAR_FB_HEIGHT * 4U);
    kv_add("Size", u.b);
    kv_add("Brightness", "Hardware Testing > Brightness (AOR)");
}

/* ---- register peek ----------------------------------------------------------- */

/* Ranges known to answer with the MMU off; anything else can stall the bus. */
static int peek_safe(uint64_t a)
{
    if (a >= 0x80000000ULL && a < 0x100000000ULL)
        return !(a >= 0xaf800000ULL && a < 0xc0080000ULL) &&      /* RKP, TIMA, secure world, seclog */
               !(a >= 0xe1900000ULL && a < 0xe3700000ULL) &&      /* secure camera */
               !(a >= 0xf0000000ULL && a < 0xf9200000ULL);        /* modem */
    if (a >= 0x880000000ULL && a < 0x900000000ULL)
        return 1;
    uint64_t pg = a & ~0xffffULL;
    switch (pg) {
    case 0x10000000: case 0x10050000: case 0x10070000:
    case 0x10400000: case 0x10410000: case 0x10430000: case 0x104b0000:
    case 0x11120000:
    case 0x14050000: case 0x14060000:
    case 0x14200000: case 0x14210000: case 0x14220000: case 0x14360000:
    case 0x1a240000: case 0x1b800000: case 0x1d000000:
        return 1;
    default:
        return 0;
    }
}

static const char *peek_key(int k)
{
    switch (k) {
    case 0: return "0"; case 1: return "1"; case 2: return "2"; case 3: return "3";
    case 4: return "4"; case 5: return "5"; case 6: return "6"; case 7: return "7";
    case 8: return "8"; case 9: return "9"; case 10: return "A"; case 11: return "B";
    case 12: return "C"; case 13: return "D"; case 14: return "E"; case 15: return "F";
    case 16: return "DEL"; case 17: return "GO"; case 18: return "WRITE";
    default: return "CLEAR";
    }
}

#define PK_Y 1380
#define PK_H 280

static void peek_draw(uint64_t addr, const char *ent, int wmode, int unlocked)
{
    struct ub u;
    ui_clip(PAGE_Y0, NAV_Y);
    ui_fill(0, PAGE_Y0, UI_W, PK_Y - PAGE_Y0, UI_BG);
    ub_init(&u);
    ub_s(&u, wmode ? "VALUE " : "ADDR ");
    ub_s(&u, "0x");
    ub_s(&u, ent[0] ? ent : "_");
    ui_text(F_TITLE, PAD, PAGE_Y0 + 100, u.b, wmode ? UI_ORANGE : UI_HOLO, UI_BG);
    ub_init(&u);
    ub_s(&u, "at ");
    ub_hex(&u, addr, 9);
    ub_s(&u, wmode ? "  " UI_DOT "  GO writes (asks first)" : "  " UI_DOT "  Vol: " UI_PLUSMINUS "0x100");
    ui_text(F_SMALL, PAD, PAGE_Y0 + 170, u.b, UI_TEXT2, UI_BG);
    ui_fill(PAD, PAGE_Y0 + 196, UI_W - 2 * PAD, 2, UI_DIV);
    uint64_t base = addr & ~0xfULL;
    int ok = peek_safe(base) || unlocked;
    for (unsigned r = 0; r < 16; ++r) {
        uint64_t a = base + 16U * r;
        ub_init(&u);
        ub_hexn(&u, a, 9);
        ub_s(&u, ": ");
        for (unsigned w = 0; w < 4; ++w) {
            uint32_t word;
            if (ok && (peek_safe(a) || unlocked) && probe32((uintptr_t)(a + 4U * w), &word))
                ub_hexn(&u, word, 8);
            else if (ok && (peek_safe(a) || unlocked))
                ub_s(&u, "*FAULT**");
            else
                ub_s(&u, "--------");
            ub_c(&u, ' ');
        }
        ui_text(F_MONO, PAD, PAGE_Y0 + 260 + 54 * (int)r, u.b, a == base ? UI_TEXT : UI_TEXT2, UI_BG);
    }
    if (!ok)
        ui_text(F_SMALL, PAD, PK_Y - 30, "Locked: this block may be powered off. GO again to read anyway.", UI_ORANGE, UI_BG);
    /* keypad: 5 rows x 4 */
    for (int k = 0; k < 20; ++k) {
        int x = (k % 4) * (UI_W / 4), y = PK_Y + (k / 4) * PK_H;
        uint32_t bg = k >= 16 ? 0xff2c2c2cU : UI_BAR;
        ui_fill(x + 4, y + 4, UI_W / 4 - 8, PK_H - 8, bg);
        ui_text_c(F_TITLE, x + UI_W / 8, y + PK_H / 2 + 20, peek_key(k),
                  k == 18 ? UI_ORANGE : k >= 16 ? UI_HOLO : UI_TEXT, bg);
    }
    ui_clip(0, UI_H);
}

static void peek_screen(void)
{
    static uint64_t addr = 0x14060000ULL;
    char ent[12];
    unsigned en = 0;
    int wmode = 0, unlocked = 0;
    ent[0] = 0;
    draw_status();
    draw_bar("Register peek", 1);
    draw_nav();
    peek_draw(addr, ent, wmode, unlocked);
    for (;;) {
        struct ev e;
        int key = -1;
        ev_wait(&e, 1000);
        if (e.type == EV_BACK)
            return;
        if (e.type == EV_HOME) {
            E.home = 1;
            return;
        }
        if (e.type == EV_UP || e.type == EV_DOWN) {
            addr = e.type == EV_UP ? addr - 0x100U : addr + 0x100U;
            unlocked = 0;
        } else if (e.type == EV_SELECT) {
            key = 17;
        } else if (e.type == EV_TAP) {
            int c = chrome_tap(&e, 1);
            if (c == 1 || c == 3)
                return;
            if (c == 2) {
                E.home = 1;
                return;
            }
            if (e.y >= PK_Y && e.y < PK_Y + 5 * PK_H)
                key = (e.y - PK_Y) / PK_H * 4 + e.x / (UI_W / 4);
        }
        if (key >= 0 && key < 16 && en < 9) {
            ent[en++] = peek_key(key)[0];
            ent[en] = 0;
        } else if (key == 16 && en) {
            ent[--en] = 0;
        } else if (key == 19) {
            en = 0;
            ent[0] = 0;
            wmode = 0;
        } else if (key == 18) {
            wmode = !wmode;
            en = 0;
            ent[0] = 0;
        } else if (key == 17) {
            uint64_t v = 0;
            for (unsigned i = 0; i < en; ++i)
                v = v * 16U + (uint64_t)(ent[i] <= '9' ? ent[i] - '0' : ent[i] - 'A' + 10);
            if (wmode && en) {
                struct ub l1, l2;
                ub_init(&l1);
                ub_s(&l1, "Write ");
                ub_hex(&l1, v & 0xffffffffU, 8);
                ub_init(&l2);
                ub_s(&l2, "to ");
                ub_hex(&l2, addr & ~3ULL, 9);
                ub_s(&l2, "? Can crash or brick.");
                if (dialog("Register write", l1.b, l2.b, "WRITE")) {
                    *(volatile uint32_t *)(uintptr_t)(addr & ~3ULL) = (uint32_t)v;
                    __asm__ volatile("dsb sy" : : : "memory");
                }
                wmode = 0;
                draw_status();
                draw_bar("Register peek", 1);
                draw_nav();
            } else if (en) {
                addr = v & ~3ULL;
                unlocked = 0;
            } else if (!peek_safe(addr & ~0xfULL)) {
                if (dialog("Unknown block", "Aborts are caught; a bus hang is not.", "Hold Power + Vol down to reset.", "READ"))
                    unlocked = 1;
                draw_status();
                draw_bar("Register peek", 1);
                draw_nav();
            }
            en = 0;
            ent[0] = 0;
        } else if (e.type == EV_NONE && !unlocked) {
            /* live refresh of known-safe blocks only */
        } else if (e.type == EV_NONE) {
            continue;
        }
        peek_draw(addr, ent, wmode, unlocked);
    }
}

static void fill_about(void)
{
    kv_big("Starbite EngineerMode", "1.2");
    kv_hdr("ABOUT");
    kv_add("Platform", "Galaxy S9 SM-G960F, Exynos 9810");
    kv_add("Runs in", "the Starbite loader (EL1, MMU off)");
    kv_add("Style", "MediaTek EngineerMode, Holo Dark");
    kv_hdr("KEYS");
    kv_add("Volume", "move");
    kv_add("Power", "select");
    kv_add("Bixby", "back (top level: next tab)");
    kv_add("Hold Bixby", "leave EngineerMode");
    kv_hdr("CREDITS");
    kv_add("Font", "Roboto (Apache 2.0)");
    kv_add("Icons", "simple-icons (CC0), Lucide (ISC)");
    kv_add("Tux", "Larry Ewing, made with The GIMP");
}

/* ======================================================================= */
/* TABS                                                                       */
/* ======================================================================= */

#define NTABS 7

static const char *tab_name(int t)
{
    switch (t) {
    case 0: return "DEVICE";
    case 1: return "HARDWARE TESTING";
    case 2: return "POWER";
    case 3: return "STORAGE";
    case 4: return "OS";
    case 5: return "LOG AND DEBUGGING";
    default: return "TOOLS";
    }
}

static void build_tab(int t)
{
    nrows = 0;
    switch (t) {
    case 0:
        row_add("Device information", "model, serials, S-Boot, Knox bit, locks", 0);
        row_add("SoC and CPU", "Exynos 9810 chip ID, core, caches, ISA", 0);
        row_add("Memory map", "DRAM banks, carve-outs, RAM per OS", 0);
        row_add("Reset and boot reason", "PMU RST_STAT, INFORM, PMIC sources", 0);
        row_add("Boot partitions", "LU0 layout: boot chain and OSes", 0);
        break;
    case 1:
        row_add("CPU", "clusters and core map", 0);
        row_add("Temperatures", "TMU: big, little, GPU, ISP (live)", 0);
        row_add("Clocks", "CPU, GPU, memory and bus PLLs (live)", 0);
        row_add("GPU (Mali-G72)", "power, voltage, ID, cores (read only)", 0);
        row_add("Core bring-up test", "PSCI CPU_ON each core, then CPU_OFF", R_WARN);
        row_add("Memory test", "16 MiB pattern test", 0);
        row_add("Memory bandwidth", "read, write, copy", 0);
        row_add("Display panel", "panel, resolution, framebuffer", 0);
        row_add("Display test", "colour fields, gradient, checkerboard, grid", 0);
        row_add("Brightness", "panel AOR via DSI, panel ID", 0);
        row_add("Touch screen", "S6SY761: paint test, controller info", 0);
        row_add("Keys", "live key states", 0);
        row_add("Real-time clock", "S2MPS18 RTC, live", 0);
        row_add("Timer", "generic timer vs RTC", 0);
        break;
    case 2:
        row_add("Battery and charger", "MAX77705 fuel gauge + charger (live)", 0);
        row_add("PMIC regulators", "S2MPS18 LDO1-45 via ACPM", 0);
        row_add("PMIC register dump", "S2MPS18 PMIC + TOP, hex", 0);
        row_add("Touch rails", "LDO35 / LDO43 on now (PMIC write)", R_WARN);
        break;
    case 3:
        row_add("UFS host controller", "UFSHCI 2.1 + Exynos vs_hci, live", 0);
        row_add("Partitions", "live GPT: every partition with size", 0);
        row_add("UFS device and health", "model, firmware, capacity, wear", 0);
        row_add("Speed test", "16 MiB sequential read (read only)", 0);
        row_add("Sector viewer", "raw LU0 blocks, hex + ASCII", 0);
        row_add("microSD card", "probe the card and its OS store (read only)", 0);
        row_add("Card store at boot", "read the card's OS store at every boot", R_WARN);
        break;
    case 4:
        row_add("Android 17", "LineageOS 24.0: store, partitions, safety", 0);
        row_add("iOS 26", "XNU 12377, boot-args", 0);
        row_add("iOS boot options", "multi-block, flags, boot iOS", 0);
        row_add("Linux 7.1", "postmarketOS (Alpine edge) on mainline", 0);
        row_add("Windows 11", "firmware, RAM stage, last run report", 0);
        row_add("OS store", "kernels on UFS: entries, sizes, CRCs", 0);
        row_add("Start an OS", "boot any installed system now", 0);
        break;
    case 5:
        row_add("Boot log", "pmsg ring at 0xFED14000", 0);
        row_add("Retained records", "RWD1, P3, latches (hex)", 0);
        row_add("Kernel command lines", "S-Boot bootargs, iOS boot-args", 0);
        row_add("Registers", "PMU, watchdog, GPIO, UFS (live)", 0);
        break;
    default:
        row_add("Reboot and power", "normal, recovery, download, power off", 0);
        row_add("Register peek", "read any block, writes ask first", R_WARN);
        row_add("About EngineerMode", "version, keys, credits", 0);
        break;
    }
}

static void open_item(int t, int i)
{
    switch (t * 16 + i) {
    case 0x00: info_screen("Device information", fill_device_info, 0); break;
    case 0x01: info_screen("SoC and CPU", fill_soc, 0); break;
    case 0x02: info_screen("Memory map", fill_memory, 0); break;
    case 0x03: info_screen("Reset and boot reason", fill_reset, 0); break;
    case 0x04: info_screen("Boot partitions", fill_partitions, 0); break;
    case 0x10: info_screen("CPU", fill_cpu, 0); break;
    case 0x11: info_screen("Temperatures", fill_thermal, 1000); break;
    case 0x12: info_screen("Clocks", fill_clocks, 2000); break;
    case 0x13: info_screen("GPU (Mali-G72)", fill_gpu, 1000); break;
    case 0x14:
        if (dialog("Core bring-up test", "Start each core, then turn it off.", "Reboot after it if a core parks.", "RUN"))
            run_cores();
        break;
    case 0x15:
        if (dialog("Memory test", "Test 16 MiB at 0x92000000?", "Takes a few seconds.", "RUN"))
            run_ram();
        break;
    case 0x16: run_bw(); break;
    case 0x17: info_screen("Display panel", fill_panel, 0); break;
    case 0x18: test_display(); break;
    case 0x19: list_screen("Brightness", build_bright, act_bright); break;
    case 0x1a: test_touch(); break;
    case 0x1b: test_keys(); break;
    case 0x1c: info_screen("Real-time clock", fill_rtc, 500); break;
    case 0x1d:
        measure_timer();
        info_screen("Timer", fill_timer, 500);
        break;
    case 0x20: info_screen("Battery and charger", fill_battery, 2000); break;
    case 0x21: info_screen("PMIC regulators", fill_pmic, 0); break;
    case 0x22: dump_pmic(); break;
    case 0x23: touch_rails(); break;
    case 0x30: info_screen("UFS host controller", fill_ufs, 1000); break;
    case 0x31: info_screen("Partitions", fill_partitions, 0); break;
    case 0x32: info_screen("UFS device and health", fill_ufs_dev, 0); break;
    case 0x33: run_speed(); break;
    case 0x34: sector_screen(); break;
    case 0x35:
        if (dialog("microSD card", "Power the card and read it?", "Read only. Hold Vol Down + Power if it hangs.", "PROBE"))
            info_screen("microSD card", fill_sd, 0);
        break;
    case 0x36: list_screen("Card store at boot", build_sdboot, act_sdboot); break;
    case 0x40: info_screen("Android 17", fill_android, 0); break;
    case 0x41: info_screen("iOS 26", fill_ios, 0); break;
    case 0x42: list_screen("iOS boot options", build_ios_opts, act_ios_opts); break;
    case 0x43: info_screen("Linux 7.1", fill_linux, 0); break;
    case 0x44: info_screen("Windows 11", fill_windows, 0); break;
    case 0x45: info_screen("OS store", fill_store, 0); break;
    case 0x46: list_screen("Start an OS", build_start, act_start); break;
    case 0x50: show_pmsg(); break;
    case 0x51: show_records(); break;
    case 0x52: show_cmdline(); break;
    case 0x53: info_screen("Registers", fill_regs, 500); break;
    case 0x60: list_screen("Reboot and power", build_reboot, act_reboot); break;
    case 0x61: peek_screen(); break;
    case 0x62: info_screen("About EngineerMode", fill_about, 0); break;
    default: break;
    }
}

static int tab_x[NTABS], tab_w[NTABS];

static void draw_tabs(void)
{
    int total = 0;
    for (int t = 0; t < NTABS; ++t) {
        tab_w[t] = ui_text_w(F_TAB, tab_name(t)) + 112;
        tab_x[t] = total;
        total += tab_w[t];
    }
    int shift = tab_x[cur_tab] + tab_w[cur_tab] / 2 - UI_W / 2;
    if (shift > total - UI_W)
        shift = total - UI_W;
    if (shift < 0)
        shift = 0;
    ui_clip(0, UI_H);
    ui_fill(0, TAB_Y, UI_W, TAB_H, UI_BAR);
    for (int t = 0; t < NTABS; ++t) {
        tab_x[t] -= shift;
        int cx = tab_x[t] + tab_w[t] / 2;
        if (tab_x[t] + tab_w[t] < 0 || tab_x[t] > UI_W)
            continue;
        ui_text_c(F_TAB, cx, TAB_Y + 86, tab_name(t), t == cur_tab ? UI_TEXT : 0xff9e9e9eU, UI_BAR);
        if (t == cur_tab)
            ui_fill(tab_x[t] + 8, TAB_Y + TAB_H - 14, tab_w[t] - 16, 14, UI_HOLO);
    }
    ui_fill(0, TAB_Y + TAB_H - 3, UI_W, 3, UI_HOLO_DIM);
}

static void draw_top(int focus, int scroll)
{
    draw_status();
    draw_bar("EngineerMode", 0);
    draw_tabs();
    draw_rows(TOP_Y0, NAV_Y, scroll, focus);
    draw_nav();
}

static int em_run(const struct star_info *in, char *cmdline, unsigned len);

/* S-Boot's vectors reset the phone on any trap: use em_vec.S while inside. */
int em_main(const struct star_info *in, char *cmdline, unsigned len)
{
    uint64_t old, mine = (uint64_t)(uintptr_t)em_vectors;
    if (!ui_init())
        return 0;
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(old));
    __asm__ volatile("msr vbar_el1, %0\n\tisb" : : "r"(mine) : "memory");
    int r = em_run(in, cmdline, len);
    __asm__ volatile("msr vbar_el1, %0\n\tisb" : : "r"(old) : "memory");
    return r;
}

static int em_run(const struct star_info *in, char *cmdline, unsigned len)
{
    E.in = in;
    E.cmd = cmdline;
    E.len = len;
    E.boot = 0;
    E.home = 0;
    E.args = 0;
    E.args_len = 0;
    if (in)
        (void)fdt_chosen_raw(in->fdt, "bootargs", &E.args, &E.args_len);
    hw_opt_load(cmdline);
    hw_tp_start();
    while (hw_keys())
        hw_wait_ms(20);
    held_keys = 0;

    int focus = 0, scroll = 0;
    build_tab(cur_tab);
    draw_top(focus, scroll);
    for (;;) {
        struct ev e;
        int view = NAV_Y - TOP_Y0, redraw = 0, open = -1, tab = cur_tab;
        ev_wait(&e, 15000);
        switch (e.type) {
        case EV_NONE:
            draw_status();
            break;
        case EV_UP:
        case EV_DOWN:
            focus = next_enabled(focus, e.type == EV_UP ? -1 : 1);
            scroll = keep_visible(focus, scroll, view);
            draw_rows(TOP_Y0, NAV_Y, scroll, focus);
            break;
        case EV_SELECT:
            open = focus;
            break;
        case EV_BACK:
        case EV_SWIPE_L:
            tab = (cur_tab + 1) % NTABS;
            break;
        case EV_SWIPE_R:
            tab = (cur_tab + NTABS - 1) % NTABS;
            break;
        case EV_HOME:
            E.home = 1;
            break;
        case EV_DRAG:
            scroll = clamp_scroll(scroll - e.d, rows_total(), view);
            draw_rows(TOP_Y0, NAV_Y, scroll, focus);
            break;
        case EV_TAP: {
            int c = chrome_tap(&e, 0);
            if (c == 2 || c == 1) {
                E.home = 1;
            } else if (c == 3) {
                tab = (cur_tab + 1) % NTABS;
            } else if (e.y >= TAB_Y && e.y < TAB_Y + TAB_H) {
                for (int t = 0; t < NTABS; ++t)
                    if (e.x >= tab_x[t] && e.x < tab_x[t] + tab_w[t])
                        tab = t;
            } else {
                int i = row_at(TOP_Y0, scroll, e.y);
                if (i >= 0 && !(rows[i].f & R_DIS)) {
                    focus = i;
                    open = i;
                }
            }
            break;
        }
        default:
            break;
        }
        if (tab != cur_tab) {
            cur_tab = tab;
            build_tab(cur_tab);
            focus = next_enabled(-1, 1);
            scroll = 0;
            draw_tabs();
            draw_rows(TOP_Y0, NAV_Y, scroll, focus);
        }
        if (open >= 0 && open < nrows && !(rows[open].f & R_DIS)) {
            draw_rows(TOP_Y0, NAV_Y, scroll, open);
            open_item(cur_tab, open);
            redraw = 1;
        }
        if (E.boot)
            return E.boot;
        if (E.home) {
            hw_tp_clear();
            while (hw_keys())
                hw_wait_ms(20);
            return 0;
        }
        if (redraw) {
            build_tab(cur_tab);
            draw_top(focus, scroll);
        }
    }
}
