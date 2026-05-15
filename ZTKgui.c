/*
 * ZTK GUI — Framebuffer Window Manager
 * Runs on Linux framebuffer (/dev/fb0) + evdev input
 * Built-in apps: 20 native + 1 web app (Zewpol)
 *
 * Compile: gcc -O2 -o ztkgui ztkgui.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>

/* ── Framebuffer globals ─────────────────────────────────── */
static int   fb_fd   = -1;
static uint32_t *fb  = NULL;
static int   fb_w    = 0;
static int   fb_h    = 0;
static size_t fb_sz  = 0;

/* ── Back-buffer for double-buffering ───────────────────── */
static uint32_t *bb  = NULL;

/* ── Color palette ──────────────────────────────────────── */
#define COL_BG          0xFF0D1117   /* desktop background    */
#define COL_PANEL       0xFF161B22   /* taskbar / panels      */
#define COL_BORDER      0xFF30363D   /* window borders        */
#define COL_TITLEBAR    0xFF21262D   /* window titlebars      */
#define COL_ACCENT      0xFF58A6FF   /* blue accent           */
#define COL_ACCENT2     0xFF3FB950   /* green                 */
#define COL_ACCENT3     0xFFD29922   /* amber                 */
#define COL_ACCENT4     0xFFF85149   /* red / close btn       */
#define COL_TEXT        0xFFE6EDF3   /* primary text          */
#define COL_MUTED       0xFF8B949E   /* secondary text        */
#define COL_WHITE       0xFFFFFFFF
#define COL_BLACK       0xFF000000
#define COL_HOVER       0xFF21262D
#define COL_WIN_BG      0xFF161B22
#define COL_INPUT_BG    0xFF0D1117
#define COL_SEL         0xFF1F6FEB   /* selection blue        */

#define TASKBAR_H       44
#define TITLEBAR_H      34
#define ICON_SIZE       64
#define ICON_COLS       6
#define ICON_PAD        20

/* ── App descriptor ─────────────────────────────────────── */
typedef struct {
    const char *name;
    const char *icon;      /* 2-char emoji-like ASCII art */
    uint32_t    color;     /* icon background color       */
    const char *binary;    /* argv[0] to exec, or NULL    */
    int         is_web;    /* 1 = open URL in w3m/links   */
    const char *url;       /* for web apps                */
    int         builtin;   /* handled internally          */
} App;

/* ── Window state ───────────────────────────────────────── */
#define MAX_WINDOWS 16
typedef struct {
    int      active;
    int      x, y, w, h;
    int      drag;       /* being dragged */
    int      drag_ox, drag_oy;
    int      minimized;
    int      maximized;
    int      app_id;
    char     title[64];
    /* app-specific state */
    char     term_buf[4096];
    int      term_pos;
    char     edit_buf[8192];
    int      edit_pos;
    char     calc_display[64];
    double   calc_val;
    int      calc_new;
    char     browser_url[256];
} Window;

static Window   wins[MAX_WINDOWS];
static int      focused_win = -1;
static int      running     = 1;

/* ── Mouse state ────────────────────────────────────────── */
static int mx = 0, my = 0, mbtn = 0;

/* ═══════════════════════════════════════════════════════════
   APP REGISTRY — 20 built-in + 1 web app (Zewpol)
   ═══════════════════════════════════════════════════════════ */
static App apps[] = {
    /* 0  */ { "Terminal",    "T>", COL_ACCENT2,  "/bin/ztksh",  0, NULL, 1 },
    /* 1  */ { "File Mgr",    "FM", COL_ACCENT,   NULL,          0, NULL, 1 },
    /* 2  */ { "Text Editor", "ED", COL_ACCENT3,  NULL,          0, NULL, 1 },
    /* 3  */ { "Calculator",  "CA", 0xFF7C3AED,   NULL,          0, NULL, 1 },
    /* 4  */ { "System Mon",  "SM", COL_ACCENT4,  NULL,          0, NULL, 1 },
    /* 5  */ { "Settings",    "ST", 0xFF6E7681,   NULL,          0, NULL, 1 },
    /* 6  */ { "Browser",     "WB", COL_ACCENT,   "/bin/links",  0, NULL, 1 },
    /* 7  */ { "Image View",  "IV", 0xFFEC4899,   NULL,          0, NULL, 1 },
    /* 8  */ { "Music Player","MP", 0xFF8B5CF6,   NULL,          0, NULL, 1 },
    /* 9  */ { "Clock",       "CL", COL_ACCENT3,  NULL,          0, NULL, 1 },
    /* 10 */ { "Calendar",    "CA", 0xFF0EA5E9,   NULL,          0, NULL, 1 },
    /* 11 */ { "Notes",       "NO", 0xFFF59E0B,   NULL,          0, NULL, 1 },
    /* 12 */ { "Network",     "NW", COL_ACCENT2,  NULL,          0, NULL, 1 },
    /* 13 */ { "Disk Usage",  "DU", 0xFFEF4444,   NULL,          0, NULL, 1 },
    /* 14 */ { "Package Mgr", "PM", 0xFF10B981,   NULL,          0, NULL, 1 },
    /* 15 */ { "Log Viewer",  "LV", 0xFF6B7280,   NULL,          0, NULL, 1 },
    /* 16 */ { "Hex Editor",  "HX", 0xFFDC2626,   NULL,          0, NULL, 1 },
    /* 17 */ { "Processes",   "PS", 0xFF7C3AED,   NULL,          0, NULL, 1 },
    /* 18 */ { "Diff Tool",   "DF", 0xFF2563EB,   NULL,          0, NULL, 1 },
    /* 19 */ { "Help",        "?",  0xFF059669,   NULL,          0, NULL, 1 },
    /* 20 */ { "Zewpol",      "ZW", 0xFF1D4ED8,   NULL,          1,
               "https://zewpol.neocities.org",    0 },
};
#define APP_COUNT 21
#define ZEWPOL_IDX 20

/* ═══════════════════════════════════════════════════════════
   DRAWING PRIMITIVES
   ═══════════════════════════════════════════════════════════ */

static inline void put_pixel(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= fb_w || y >= fb_h) return;
    bb[y * fb_w + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            put_pixel(i, j, c);
}

static void draw_rect(int x, int y, int w, int h, uint32_t c) {
    for (int i = x; i < x + w; i++) {
        put_pixel(i, y,       c);
        put_pixel(i, y+h-1,   c);
    }
    for (int j = y; j < y + h; j++) {
        put_pixel(x,     j,   c);
        put_pixel(x+w-1, j,   c);
    }
}

static void draw_circle(int cx, int cy, int r, uint32_t c) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i*i + j*j <= r*r)
                put_pixel(cx+i, cy+j, c);
}

/* 5x7 bitmap font — 96 ASCII chars starting at space (0x20) */
static const uint8_t font5x7[96][7] = {
 {0,0,0,0,0,0,0},{0x04,0x04,0x04,0x04,0,0x04,0}, /* sp ! */
 {0x0A,0x0A,0,0,0,0,0},{0x0A,0x1F,0x0A,0x1F,0x0A,0,0}, /* " # */
 {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* $ */
 {0x18,0x19,0x02,0x04,0x13,0x03,0},    /* % */
 {0x08,0x14,0x14,0x08,0x15,0x12,0x0D},/* & */
 {0x04,0x04,0,0,0,0,0},                /* ' */
 {0x02,0x04,0x08,0x08,0x08,0x04,0x02},/* ( */
 {0x08,0x04,0x02,0x02,0x02,0x04,0x08},/* ) */
 {0,0x0A,0x04,0x1F,0x04,0x0A,0},      /* * */
 {0,0x04,0x04,0x1F,0x04,0x04,0},      /* + */
 {0,0,0,0,0,0x04,0x08},               /* , */
 {0,0,0,0x1F,0,0,0},                  /* - */
 {0,0,0,0,0,0x04,0},                  /* . */
 {0x01,0x01,0x02,0x04,0x08,0x10,0x10},/* / */
 {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},/* 0 */
 {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},/* 1 */
 {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},/* 2 */
 {0x1F,0x02,0x04,0x06,0x01,0x11,0x0E},/* 3 */
 {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},/* 4 */
 {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},/* 5 */
 {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},/* 6 */
 {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},/* 7 */
 {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},/* 8 */
 {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},/* 9 */
 {0,0x04,0,0,0,0x04,0},               /* : */
 {0,0x04,0,0,0,0x04,0x08},            /* ; */
 {0x02,0x04,0x08,0x10,0x08,0x04,0x02},/* < */
 {0,0,0x1F,0,0x1F,0,0},              /* = */
 {0x08,0x04,0x02,0x01,0x02,0x04,0x08},/* > */
 {0x0E,0x11,0x01,0x06,0x04,0,0x04},  /* ? */
 {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},/* @ */
 {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},/* A */
 {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},/* B */
 {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},/* C */
 {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},/* D */
 {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},/* E */
 {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},/* F */
 {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},/* G */
 {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},/* H */
 {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},/* I */
 {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},/* J */
 {0x11,0x12,0x14,0x18,0x14,0x12,0x11},/* K */
 {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},/* L */
 {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},/* M */
 {0x11,0x19,0x15,0x13,0x11,0x11,0x11},/* N */
 {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},/* O */
 {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},/* P */
 {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},/* Q */
 {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},/* R */
 {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},/* S */
 {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},/* T */
 {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},/* U */
 {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},/* V */
 {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},/* W */
 {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},/* X */
 {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},/* Y */
 {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},/* Z */
 {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},/* [ */
 {0x10,0x10,0x08,0x04,0x02,0x01,0x01},/* \ */
 {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},/* ] */
 {0x04,0x0A,0x11,0,0,0,0},           /* ^ */
 {0,0,0,0,0,0,0x1F},                 /* _ */
 {0x08,0x04,0,0,0,0,0},              /* ` */
 {0,0,0x0E,0x01,0x0F,0x11,0x0F},    /* a */
 {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E},/* b */
 {0,0,0x0E,0x10,0x10,0x11,0x0E},    /* c */
 {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F},/* d */
 {0,0,0x0E,0x11,0x1F,0x10,0x0E},    /* e */
 {0x06,0x08,0x1E,0x08,0x08,0x08,0x08},/* f */
 {0,0,0x0F,0x11,0x0F,0x01,0x0E},    /* g */
 {0x10,0x10,0x16,0x19,0x11,0x11,0x11},/* h */
 {0x04,0,0x04,0x04,0x04,0x04,0x04}, /* i */
 {0x02,0,0x02,0x02,0x02,0x12,0x0C}, /* j */
 {0x10,0x10,0x12,0x14,0x18,0x14,0x12},/* k */
 {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},/* l */
 {0,0,0x1A,0x15,0x15,0x11,0x11},    /* m */
 {0,0,0x16,0x19,0x11,0x11,0x11},    /* n */
 {0,0,0x0E,0x11,0x11,0x11,0x0E},    /* o */
 {0,0,0x1E,0x11,0x1E,0x10,0x10},    /* p */
 {0,0,0x0F,0x11,0x0F,0x01,0x01},    /* q */
 {0,0,0x16,0x19,0x10,0x10,0x10},    /* r */
 {0,0,0x0E,0x10,0x0E,0x01,0x1E},    /* s */
 {0x08,0x08,0x1E,0x08,0x08,0x08,0x06},/* t */
 {0,0,0x11,0x11,0x11,0x13,0x0D},    /* u */
 {0,0,0x11,0x11,0x11,0x0A,0x04},    /* v */
 {0,0,0x11,0x15,0x15,0x15,0x0A},    /* w */
 {0,0,0x11,0x0A,0x04,0x0A,0x11},    /* x */
 {0,0,0x11,0x11,0x0F,0x01,0x0E},    /* y */
 {0,0,0x1F,0x02,0x04,0x08,0x1F},    /* z */
 {0x06,0x04,0x04,0x08,0x04,0x04,0x06},/* { */
 {0x04,0x04,0x04,0,0x04,0x04,0x04}, /* | */
 {0x0C,0x04,0x04,0x02,0x04,0x04,0x0C},/* } */
 {0x08,0x15,0x02,0,0,0,0},          /* ~ */
 {0,0,0,0,0,0,0},                   /* del */
};

static void draw_char(int x, int y, char ch, uint32_t fg, int scale) {
    if (ch < 0x20 || ch > 0x7E) return;
    const uint8_t *bm = font5x7[(uint8_t)ch - 0x20];
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (bm[row] & (1 << (4 - col))) {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        put_pixel(x + col*scale + sx, y + row*scale + sy, fg);
            }
        }
    }
}

static void draw_string(int x, int y, const char *s, uint32_t fg, int scale) {
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { y += 8*scale; cx = x; continue; }
        draw_char(cx, y, *s, fg, scale);
        cx += 6*scale;
    }
}

static int str_width(const char *s, int scale) {
    return (int)strlen(s) * 6 * scale;
}

/* ── Rounded rect ───────────────────────────────────────── */
static void fill_rounded(int x, int y, int w, int h, int r, uint32_t c) {
    fill_rect(x+r, y, w-2*r, h, c);
    fill_rect(x, y+r, r, h-2*r, c);
    fill_rect(x+w-r, y+r, r, h-2*r, c);
    draw_circle(x+r,     y+r,     r, c);
    draw_circle(x+w-r-1, y+r,     r, c);
    draw_circle(x+r,     y+h-r-1, r, c);
    draw_circle(x+w-r-1, y+h-r-1, r, c);
}

/* ═══════════════════════════════════════════════════════════
   WINDOW MANAGER
   ═══════════════════════════════════════════════════════════ */

static int new_window(int app_id, int x, int y, int w, int h) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].active) {
            memset(&wins[i], 0, sizeof(Window));
            wins[i].active  = 1;
            wins[i].x       = x;
            wins[i].y       = y;
            wins[i].w       = w;
            wins[i].h       = h;
            wins[i].app_id  = app_id;
            snprintf(wins[i].title, 64, "%s", apps[app_id].name);
            wins[i].calc_new = 1;
            snprintf(wins[i].calc_display, 64, "0");
            focused_win = i;
            return i;
        }
    }
    return -1;
}

static void close_window(int id) {
    wins[id].active = 0;
    focused_win = -1;
    for (int i = MAX_WINDOWS-1; i >= 0; i--)
        if (wins[i].active) { focused_win = i; break; }
}

/* ── Draw window chrome ─────────────────────────────────── */
static void draw_window(int id) {
    Window *w = &wins[id];
    if (!w->active || w->minimized) return;

    int x = w->x, y = w->y, wd = w->w, ht = w->h;
    int focused = (id == focused_win);

    /* Shadow */
    fill_rounded(x+4, y+4, wd, ht, 8, 0x44000000);

    /* Window background */
    fill_rounded(x, y, wd, ht, 8, COL_WIN_BG);

    /* Border */
    uint32_t bc = focused ? COL_ACCENT : COL_BORDER;
    draw_rect(x, y, wd, ht, bc);

    /* Titlebar */
    fill_rect(x+1, y+1, wd-2, TITLEBAR_H, COL_TITLEBAR);

    /* WM buttons */
    draw_circle(x+14, y+TITLEBAR_H/2, 6, COL_ACCENT4);  /* close  */
    draw_circle(x+30, y+TITLEBAR_H/2, 6, COL_ACCENT3);  /* min    */
    draw_circle(x+46, y+TITLEBAR_H/2, 6, COL_ACCENT2);  /* max    */

    /* Title */
    int tw = str_width(w->title, 1);
    draw_string(x + (wd - tw)/2, y + (TITLEBAR_H - 7)/2, w->title, COL_MUTED, 1);

    /* App content area */
    int cx = x+2, cy = y+TITLEBAR_H+2;
    int cw = wd-4, ch = ht-TITLEBAR_H-4;

    switch (w->app_id) {

    /* ── 0: Terminal ──────────────────────────────────── */
    case 0:
        fill_rect(cx, cy, cw, ch, 0xFF0A0E13);
        draw_string(cx+6, cy+6, "ZTK Terminal v1.0", COL_ACCENT2, 1);
        draw_string(cx+6, cy+18, "─────────────────────────", COL_BORDER, 1);
        draw_string(cx+6, cy+32, w->term_buf, COL_TEXT, 1);
        /* blinking cursor */
        {
            int rows = w->term_pos / (cw/6);
            int cols = w->term_pos % (cw/6);
            int blink = (int)(clock() / (CLOCKS_PER_SEC/2)) % 2;
            if (blink)
                fill_rect(cx+6+cols*6, cy+32+rows*10, 6, 8, COL_ACCENT2);
        }
        draw_string(cx+6, cy+ch-14, "ztk@localhost:~$ _", COL_ACCENT2, 1);
        break;

    /* ── 1: File Manager ─────────────────────────────── */
    case 1:
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        draw_string(cx+6, cy+6, "/ (root filesystem)", COL_ACCENT, 1);
        draw_string(cx+6, cy+20, "─────────────────────────────", COL_BORDER, 1);
        {
            const char *entries[] = {
                "[D] bin/", "[D] boot/", "[D] dev/", "[D] etc/",
                "[D] home/", "[D] lib/", "[D] proc/", "[D] sys/",
                "[D] tmp/", "[D] usr/", "[D] var/", "[F] vmlinuz"
            };
            for (int i = 0; i < 12; i++) {
                uint32_t fc = entries[i][1]=='D' ? COL_ACCENT : COL_TEXT;
                draw_string(cx+6, cy+34+i*12, entries[i], fc, 1);
            }
        }
        break;

    /* ── 2: Text Editor ──────────────────────────────── */
    case 2:
        fill_rect(cx, cy, cw, ch, 0xFF0D1117);
        draw_string(cx+6, cy+4, "ZTK Edit — untitled.txt", COL_MUTED, 1);
        draw_string(cx+6, cy+16, "─────────────────────────────", COL_BORDER, 1);
        if (strlen(w->edit_buf) == 0)
            draw_string(cx+6, cy+28, "Start typing...", COL_MUTED, 1);
        else
            draw_string(cx+6, cy+28, w->edit_buf, COL_TEXT, 1);
        draw_string(cx+6, cy+ch-12, "Ln 1, Col 1  |  UTF-8  |  ZTK Edit", COL_MUTED, 1);
        break;

    /* ── 3: Calculator ───────────────────────────────── */
    case 3: {
        fill_rect(cx, cy, cw, ch, 0xFF1A0E2E);
        /* Display */
        fill_rect(cx+6, cy+6, cw-12, 28, 0xFF0D0718);
        draw_rect(cx+6, cy+6, cw-12, 28, 0xFF7C3AED);
        {
            int dw = str_width(w->calc_display, 2);
            draw_string(cx+cw-dw-10, cy+13, w->calc_display, COL_WHITE, 2);
        }
        /* Buttons */
        const char *btns[] = {
            "C","(",")","÷",
            "7","8","9","×",
            "4","5","6","-",
            "1","2","3","+",
            "±","0",".","="
        };
        uint32_t btn_colors[] = {
            COL_ACCENT4, COL_MUTED, COL_MUTED, 0xFF7C3AED,
            COL_HOVER,COL_HOVER,COL_HOVER, 0xFF7C3AED,
            COL_HOVER,COL_HOVER,COL_HOVER, 0xFF7C3AED,
            COL_HOVER,COL_HOVER,COL_HOVER, COL_ACCENT2,
            COL_HOVER,COL_HOVER,COL_HOVER, COL_ACCENT
        };
        int bw = (cw-12)/4 - 3, bh = (ch-50)/5 - 3;
        for (int i = 0; i < 20; i++) {
            int col = i % 4, row = i / 4;
            int bx = cx+6 + col*(bw+3);
            int by = cy+40 + row*(bh+3);
            fill_rounded(bx, by, bw, bh, 5, btn_colors[i]);
            int tw2 = str_width(btns[i], 1);
            draw_string(bx+(bw-tw2)/2, by+(bh-7)/2, btns[i], COL_TEXT, 1);
        }
        break;
    }

    /* ── 4: System Monitor ───────────────────────────── */
    case 4:
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        draw_string(cx+6, cy+6, "ZTK System Monitor", COL_ACCENT4, 1);
        draw_string(cx+6, cy+20, "──────────────────────────────────", COL_BORDER, 1);
        {
            /* Fake CPU bar */
            int cpu = 34;
            draw_string(cx+6, cy+32, "CPU Usage:", COL_MUTED, 1);
            fill_rect(cx+75, cy+30, (cw-85)*cpu/100, 10, COL_ACCENT4);
            fill_rect(cx+75+(cw-85)*cpu/100, cy+30,
                      (cw-85)*(100-cpu)/100, 10, COL_HOVER);
            char cbuf[16]; snprintf(cbuf, 16, "%d%%", cpu);
            draw_string(cx+cw-30, cy+32, cbuf, COL_TEXT, 1);

            /* Fake RAM bar */
            int ram = 61;
            draw_string(cx+6, cy+50, "RAM Usage:", COL_MUTED, 1);
            fill_rect(cx+75, cy+48, (cw-85)*ram/100, 10, COL_ACCENT3);
            fill_rect(cx+75+(cw-85)*ram/100, cy+48,
                      (cw-85)*(100-ram)/100, 10, COL_HOVER);
            snprintf(cbuf, 16, "%d%%", ram);
            draw_string(cx+cw-30, cy+50, cbuf, COL_TEXT, 1);

            /* Processes */
            draw_string(cx+6, cy+70, "─── Top Processes ───────────────────", COL_BORDER, 1);
            const char *procs[] = {
                "ztkgui    12.3%   48MB",
                "ztksh      2.1%    6MB",
                "init       0.1%    1MB",
                "syslogd    0.0%    2MB",
                "kworker    0.3%    0MB",
            };
            for (int i = 0; i < 5; i++)
                draw_string(cx+6, cy+84+i*12, procs[i], COL_TEXT, 1);
        }
        break;

    /* ── 5: Settings ─────────────────────────────────── */
    case 5:
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        draw_string(cx+6, cy+6, "ZTK Settings", COL_TEXT, 1);
        draw_string(cx+6, cy+20, "──────────────────────────────────", COL_BORDER, 1);
        {
            const char *sections[] = {
                "> Display & Wallpaper",
                "> Network & Wi-Fi",
                "> Sound & Audio",
                "> Users & Accounts",
                "> Date & Time",
                "> Keyboard & Mouse",
                "> Power Management",
                "> Security & Privacy",
                "> About ZTK OS",
            };
            for (int i = 0; i < 9; i++) {
                fill_rounded(cx+6, cy+32+i*18, cw-12, 15, 4, COL_HOVER);
                draw_string(cx+12, cy+35+i*18, sections[i], COL_TEXT, 1);
            }
        }
        break;

    /* ── 6: Browser ──────────────────────────────────── */
    case 6:
        fill_rect(cx, cy, cw, ch, 0xFFF8F9FA);
        fill_rect(cx, cy, cw, 28, COL_TITLEBAR);
        draw_string(cx+6, cy+10, "URL: https://zewpol.neocities.org", COL_MUTED, 1);
        draw_string(cx+6, cy+36, "ZTK Browser — Loading...", COL_BLACK, 1);
        draw_string(cx+6, cy+52, "Open Zewpol from the desktop to visit", 0xFF333333, 1);
        draw_string(cx+6, cy+66, "the pinned web app directly.", 0xFF333333, 1);
        break;

    /* ── 7: Image Viewer ─────────────────────────────── */
    case 7:
        fill_rect(cx, cy, cw, ch, 0xFF0A0A0A);
        draw_string(cx+6, cy+6, "ZTK Image Viewer", 0xFFEC4899, 1);
        /* Fake image placeholder */
        fill_rect(cx+cw/2-60, cy+ch/2-45, 120, 90, COL_HOVER);
        draw_rect(cx+cw/2-60, cy+ch/2-45, 120, 90, COL_BORDER);
        draw_string(cx+cw/2-24, cy+ch/2-4, "No Image", COL_MUTED, 1);
        draw_string(cx+6, cy+ch-12, "Drag & drop an image file here", COL_MUTED, 1);
        break;

    /* ── 8: Music Player ─────────────────────────────── */
    case 8:
        fill_rect(cx, cy, cw, ch, 0xFF120820);
        draw_string(cx+6, cy+6, "ZTK Music", 0xFF8B5CF6, 1);
        /* Album art placeholder */
        fill_rounded(cx+cw/2-40, cy+16, 80, 80, 8, 0xFF2D1B69);
        draw_string(cx+cw/2-6, cy+50, "♪", 0xFF8B5CF6, 2);
        draw_string(cx+6, cy+106, "  No Track Loaded", COL_MUTED, 1);
        draw_string(cx+6, cy+120, "  ────────────────────────────", COL_BORDER, 1);
        /* Playback bar */
        fill_rect(cx+6, cy+134, cw-12, 6, COL_HOVER);
        fill_rect(cx+6, cy+134, (cw-12)/3, 6, 0xFF8B5CF6);
        draw_string(cx+6, cy+146, "  |<<   <<   ▶   >>   >>|", 0xFF8B5CF6, 1);
        break;

    /* ── 9: Clock ────────────────────────────────────── */
    case 9: {
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        char tbuf[32];
        snprintf(tbuf, 32, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
        int tw3 = str_width(tbuf, 3);
        draw_string(cx+(cw-tw3)/2, cy+ch/2-12, tbuf, COL_ACCENT, 3);
        char dbuf[32];
        snprintf(dbuf, 32, "%04d-%02d-%02d",
                 tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday);
        int dw2 = str_width(dbuf, 1);
        draw_string(cx+(cw-dw2)/2, cy+ch/2+14, dbuf, COL_MUTED, 1);
        break;
    }

    /* ── 10: Calendar ────────────────────────────────── */
    case 10: {
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        char hdr[32];
        const char *months[] = {"January","February","March","April",
            "May","June","July","August","September","October","November","December"};
        snprintf(hdr, 32, "%s %d", months[tm->tm_mon], tm->tm_year+1900);
        int hw = str_width(hdr, 1);
        draw_string(cx+(cw-hw)/2, cy+6, hdr, COL_ACCENT, 1);
        draw_string(cx+6, cy+22, "Su Mo Tu We Th Fr Sa", COL_MUTED, 1);
        draw_string(cx+6, cy+34, "───────────────────────", COL_BORDER, 1);
        /* Simple calendar grid */
        int dow = (tm->tm_wday - tm->tm_mday%7 + 14) % 7;
        int day = 1;
        int days_in_month = 31;
        for (int row = 0; row < 6 && day <= days_in_month; row++) {
            for (int col = 0; col < 7 && day <= days_in_month; col++) {
                if (row == 0 && col < dow) continue;
                char dstr[4]; snprintf(dstr, 4, "%2d", day);
                uint32_t dc = (day == tm->tm_mday) ? COL_ACCENT : COL_TEXT;
                if (day == tm->tm_mday)
                    fill_rounded(cx+6+col*18-1, cy+46+row*14-1, 14, 11, 3, COL_SEL);
                draw_string(cx+6+col*18, cy+46+row*14, dstr, dc, 1);
                day++;
            }
        }
        break;
    }

    /* ── 11: Notes ───────────────────────────────────── */
    case 11:
        fill_rect(cx, cy, cw, ch, 0xFF1A1500);
        draw_string(cx+6, cy+6, "ZTK Notes", 0xFFF59E0B, 1);
        draw_string(cx+6, cy+20, "─────────────────────────────────", 0xFF3D2E00, 1);
        if (strlen(w->edit_buf) == 0)
            draw_string(cx+6, cy+32, "New note... (type to begin)", COL_MUTED, 1);
        else
            draw_string(cx+6, cy+32, w->edit_buf, 0xFFFDE68A, 1);
        break;

    /* ── 12: Network ─────────────────────────────────── */
    case 12:
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        draw_string(cx+6, cy+6, "ZTK Network Manager", COL_ACCENT2, 1);
        draw_string(cx+6, cy+20, "──────────────────────────────────────", COL_BORDER, 1);
        {
            const char *ifaces[] = {
                "eth0    UP    192.168.1.100/24",
                "lo      UP    127.0.0.1/8",
                "wlan0   DOWN  (not connected)",
            };
            for (int i = 0; i < 3; i++) {
                uint32_t sc = (i==2) ? COL_ACCENT4 : COL_ACCENT2;
                draw_string(cx+6, cy+34+i*14, ifaces[i], sc, 1);
            }
            draw_string(cx+6, cy+82, "DNS: 1.1.1.1  8.8.8.8", COL_MUTED, 1);
            draw_string(cx+6, cy+96, "Gateway: 192.168.1.1", COL_MUTED, 1);
        }
        break;

    /* ── 13: Disk Usage ──────────────────────────────── */
    case 13:
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        draw_string(cx+6, cy+6, "ZTK Disk Usage", COL_ACCENT4, 1);
        draw_string(cx+6, cy+20, "──────────────────────────────────────", COL_BORDER, 1);
        {
            const char *mounts[] = {"/","/boot","/home","/tmp"};
            int usages[] = {42, 71, 18, 5};
            for (int i = 0; i < 4; i++) {
                draw_string(cx+6, cy+32+i*22, mounts[i], COL_TEXT, 1);
                fill_rect(cx+50, cy+30+i*22, (cw-60)*usages[i]/100, 10, COL_ACCENT4);
                fill_rect(cx+50+(cw-60)*usages[i]/100, cy+30+i*22,
                          (cw-60)*(100-usages[i])/100, 10, COL_HOVER);
                char ubuf[8]; snprintf(ubuf, 8, "%d%%", usages[i]);
                draw_string(cx+cw-25, cy+32+i*22, ubuf, COL_MUTED, 1);
            }
        }
        break;

    /* ── 14: Package Manager ─────────────────────────── */
    case 14:
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        draw_string(cx+6, cy+6, "ZTK Package Manager (zpkg)", 0xFF10B981, 1);
        draw_string(cx+6, cy+20, "──────────────────────────────────────", COL_BORDER, 1);
        {
            const char *pkgs[] = {
                "[i] busybox      1.36.1   Core utilities",
                "[i] linux        6.6.30   The kernel",
                "[i] ztkgui       1.0.0    Desktop GUI",
                "[i] ztksh        1.0.0    ZTK shell",
                "[ ] vim          9.1.0    Text editor",
                "[ ] python3      3.12.0   Python runtime",
                "[ ] links        2.29     CLI browser",
            };
            for (int i = 0; i < 7; i++) {
                uint32_t pc = (pkgs[i][1]=='i') ? 0xFF10B981 : COL_MUTED;
                draw_string(cx+6, cy+34+i*13, pkgs[i], pc, 1);
            }
            draw_string(cx+6, cy+ch-12, "zpkg install <name>  to install", COL_MUTED, 1);
        }
        break;

    /* ── 15: Log Viewer ──────────────────────────────── */
    case 15:
        fill_rect(cx, cy, cw, ch, 0xFF080808);
        draw_string(cx+6, cy+6, "/var/log/ztk.log", COL_MUTED, 1);
        draw_string(cx+6, cy+18, "──────────────────────────────────────", 0xFF222222, 1);
        {
            const char *logs[] = {
                "[  0.000] ZTK kernel 6.6.30 booting...",
                "[  0.021] Detected x86_64 CPU, 2 cores",
                "[  0.048] Mounting root filesystem OK",
                "[  0.089] Starting ZTK init system...",
                "[  0.102] Network: eth0 UP 192.168.1.100",
                "[  0.134] Starting ztkgui framebuffer...",
                "[  0.201] Desktop ready. Welcome to ZTK!",
            };
            for (int i = 0; i < 7; i++) {
                uint32_t lc = (i==6) ? COL_ACCENT2 : COL_MUTED;
                draw_string(cx+6, cy+30+i*12, logs[i], lc, 1);
            }
        }
        break;

    /* ── 16: Hex Editor ──────────────────────────────── */
    case 16:
        fill_rect(cx, cy, cw, ch, 0xFF050A05);
        draw_string(cx+6, cy+6, "ZTK Hex Editor — /bin/ztksh", 0xFFDC2626, 1);
        draw_string(cx+6, cy+18, "──────────────────────────────────────", 0xFF1A0000, 1);
        {
            /* Fake hex dump */
            const char *rows[] = {
                "00000000  7F 45 4C 46 02 01 01 00  ELF.....",
                "00000008  00 00 00 00 00 00 00 00  ........",
                "00000010  02 00 3E 00 01 00 00 00  ..>.....",
                "00000018  78 10 40 00 00 00 00 00  x.@.....",
                "00000020  40 00 00 00 00 00 00 00  @.......",
            };
            for (int i = 0; i < 5; i++)
                draw_string(cx+6, cy+30+i*12, rows[i], 0xFF22C55E, 1);
        }
        break;

    /* ── 17: Process Viewer ──────────────────────────── */
    case 17:
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        draw_string(cx+6, cy+6, "PID   PPID  CPU  MEM  CMD", COL_ACCENT, 1);
        draw_string(cx+6, cy+18, "──────────────────────────────────────", COL_BORDER, 1);
        {
            const char *ps[] = {
                "    1     0   0.0  0.1  init",
                "    2     0   0.0  0.0  kthreadd",
                "   47     1   0.1  0.2  syslogd",
                "   89     1  12.3 48.2  ztkgui",
                "   91    89   2.1  6.1  ztksh",
                "   94    89   0.4  2.3  ztksh [child]",
                "  102     1   0.0  0.1  udevd",
            };
            for (int i = 0; i < 7; i++)
                draw_string(cx+6, cy+30+i*12, ps[i], COL_TEXT, 1);
        }
        break;

    /* ── 18: Diff Tool ───────────────────────────────── */
    case 18:
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        draw_string(cx+6, cy+6, "ZTK Diff — file_a.txt vs file_b.txt", 0xFF2563EB, 1);
        draw_string(cx+6, cy+18, "──────────────────────────────────────", COL_BORDER, 1);
        {
            const char *lines[] = {
                "  1:  Hello, world!",
                "  2:  This is ZTK OS.",
                "- 3:  Old line removed",
                "+ 3:  New line added",
                "  4:  End of file.",
            };
            uint32_t lcs[] = {COL_TEXT,COL_TEXT,COL_ACCENT4,COL_ACCENT2,COL_TEXT};
            for (int i = 0; i < 5; i++)
                draw_string(cx+6, cy+30+i*13, lines[i], lcs[i], 1);
        }
        break;

    /* ── 19: Help ────────────────────────────────────── */
    case 19:
        fill_rect(cx, cy, cw, ch, COL_WIN_BG);
        draw_string(cx+6, cy+6, "ZTK OS Help & Documentation", 0xFF059669, 1);
        draw_string(cx+6, cy+20, "──────────────────────────────────────", COL_BORDER, 1);
        {
            const char *help[] = {
                "Double-click desktop icons to open apps",
                "Drag titlebars to move windows",
                "Click X to close, - to minimize",
                "Right-click desktop: context menu",
                "Ctrl+Alt+T: open terminal",
                "Ctrl+Alt+F: open file manager",
                "",
                "ZTK Shell commands:",
                "  ls, cd, cat, echo, ps, top",
                "  zpkg install/remove <pkg>",
                "  ztkgui --restart",
            };
            for (int i = 0; i < 11; i++)
                draw_string(cx+6, cy+32+i*12, help[i], COL_TEXT, 1);
        }
        break;

    /* ── 20: Zewpol (web app) ────────────────────────── */
    case 20:
        fill_rect(cx, cy, cw, ch, 0xFF0F172A);
        /* Header bar */
        fill_rect(cx, cy, cw, 26, 0xFF1E293B);
        draw_string(cx+6, cy+9, "https://zewpol.neocities.org", 0xFF93C5FD, 1);
        /* Page body placeholder */
        draw_string(cx+6, cy+36, "╔═══════════════════════════════╗", 0xFF1D4ED8, 1);
        draw_string(cx+6, cy+48, "║        Z E W P O L            ║", COL_WHITE, 1);
        draw_string(cx+6, cy+60, "╚═══════════════════════════════╝", 0xFF1D4ED8, 1);
        draw_string(cx+6, cy+76, "Pinned web app — opens in ZTK Browser", COL_MUTED, 1);
        draw_string(cx+6, cy+90, "or via 'links https://zewpol.neocities.org'", COL_MUTED, 1);
        /* Launch button */
        fill_rounded(cx+cw/2-50, cy+ch-36, 100, 22, 6, 0xFF1D4ED8);
        draw_string(cx+cw/2-24, cy+ch-30, "Open Zewpol", COL_WHITE, 1);
        break;

    default: break;
    }
}

/* ═══════════════════════════════════════════════════════════
   DESKTOP RENDERER
   ═══════════════════════════════════════════════════════════ */

static void draw_desktop(void) {
    /* Wallpaper — dark grid */
    fill_rect(0, 0, fb_w, fb_h, COL_BG);
    for (int y = 0; y < fb_h; y += 40)
        for (int x = 0; x < fb_w; x++)
            put_pixel(x, y, 0xFF111820);
    for (int x = 0; x < fb_w; x += 40)
        for (int y = 0; y < fb_h; y++)
            put_pixel(x, y, 0xFF111820);

    /* ZTK logo watermark */
    draw_string(fb_w/2-18, fb_h/2-8, "ZTK", 0xFF1A2030, 3);

    /* Desktop icons */
    int cols = ICON_COLS;
    int start_x = fb_w - cols*(ICON_SIZE+ICON_PAD) - 10;
    int start_y = 16;
    for (int i = 0; i < APP_COUNT; i++) {
        int col = i % cols;
        int row = i / cols;
        int ix = start_x + col*(ICON_SIZE+ICON_PAD);
        int iy = start_y + row*(ICON_SIZE+ICON_PAD+16);

        /* Special Zewpol pinned indicator */
        if (i == ZEWPOL_IDX) {
            draw_rect(ix-3, iy-3, ICON_SIZE+6, ICON_SIZE+6, 0xFF1D4ED8);
        }

        fill_rounded(ix, iy, ICON_SIZE, ICON_SIZE, 10, apps[i].color);
        /* Icon label (2 chars) */
        int lw = str_width(apps[i].icon, 2);
        draw_string(ix+(ICON_SIZE-lw)/2, iy+(ICON_SIZE-14)/2, apps[i].icon, COL_WHITE, 2);

        /* App name below icon */
        int nw = str_width(apps[i].name, 1);
        draw_string(ix+(ICON_SIZE-nw)/2, iy+ICON_SIZE+3, apps[i].name, COL_TEXT, 1);

        /* Zewpol: pinned badge */
        if (i == ZEWPOL_IDX) {
            fill_rounded(ix+ICON_SIZE-18, iy-4, 22, 12, 4, 0xFF1D4ED8);
            draw_string(ix+ICON_SIZE-16, iy-2, "PIN", COL_WHITE, 1);
        }
    }
}

static void draw_taskbar(void) {
    fill_rect(0, fb_h-TASKBAR_H, fb_w, TASKBAR_H, COL_TASKBAR);
    draw_rect(0, fb_h-TASKBAR_H, fb_w, 1, COL_BORDER);

    /* Start button */
    fill_rounded(8, fb_h-TASKBAR_H+8, 50, 28, 5, COL_ACCENT);
    draw_string(14, fb_h-TASKBAR_H+16, "ZTK", COL_WHITE, 1);

    /* App buttons for open windows */
    int bx = 66;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].active || wins[i].minimized) continue;
        uint32_t bc = (i == focused_win) ? COL_SEL : COL_HOVER;
        fill_rounded(bx, fb_h-TASKBAR_H+8, 100, 28, 5, bc);
        draw_string(bx+6, fb_h-TASKBAR_H+16, wins[i].title, COL_TEXT, 1);
        bx += 108;
    }

    /* Clock */
    {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        char tbuf[16];
        snprintf(tbuf, 16, "%02d:%02d", tm->tm_hour, tm->tm_min);
        int tw = str_width(tbuf, 1);
        draw_string(fb_w-tw-12, fb_h-TASKBAR_H+18, tbuf, COL_MUTED, 1);
    }

    /* Status dots */
    draw_circle(fb_w-70, fb_h-TASKBAR_H+22, 4, COL_ACCENT2);  /* net  */
    draw_circle(fb_w-58, fb_h-TASKBAR_H+22, 4, COL_ACCENT3);  /* vol  */
    draw_circle(fb_w-46, fb_h-TASKBAR_H+22, 4, COL_ACCENT);   /* sys  */
}

static void draw_cursor(void) {
    /* Arrow cursor */
    for (int i = 0; i < 12; i++) {
        put_pixel(mx,   my+i, COL_WHITE);
        put_pixel(mx+1, my+i, COL_WHITE);
    }
    for (int i = 0; i < 8; i++) {
        put_pixel(mx+i, my,   COL_WHITE);
        put_pixel(mx+i, my+1, COL_WHITE);
    }
}

static void flip_buffer(void) {
    memcpy(fb, bb, fb_sz);
}

/* ═══════════════════════════════════════════════════════════
   LAUNCH APP
   ═══════════════════════════════════════════════════════════ */

static void launch_app(int app_id) {
    /* Check if already open */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wins[i].active && wins[i].app_id == app_id) {
            focused_win = i;
            wins[i].minimized = 0;
            return;
        }
    }

    if (apps[app_id].is_web) {
        /* Fork links/w3m for web app */
        pid_t pid = fork();
        if (pid == 0) {
            char cmd[512];
            snprintf(cmd, 512, "links %s 2>/dev/null || "
                     "w3m %s 2>/dev/null || "
                     "echo 'Install links: zpkg install links'",
                     apps[app_id].url, apps[app_id].url);
            execl("/bin/sh", "sh", "-c", cmd, NULL);
            exit(0);
        }
    }

    /* Create window */
    int wx = 80 + (app_id * 22) % 200;
    int wy = 60 + (app_id * 18) % 100;
    int ww = 480, wh = 320;

    /* Size overrides */
    if (app_id == 3)  { ww = 240; wh = 300; }  /* calculator */
    if (app_id == 9)  { ww = 300; wh = 120; }  /* clock       */
    if (app_id == 10) { ww = 260; wh = 200; }  /* calendar    */
    if (app_id == 20) { ww = 520; wh = 340; }  /* zewpol      */

    new_window(app_id, wx, wy, ww, wh);
}

/* ═══════════════════════════════════════════════════════════
   INPUT HANDLING
   ═══════════════════════════════════════════════════════════ */

static void handle_click(int x, int y) {
    /* Check WM buttons on focused window */
    if (focused_win >= 0) {
        Window *w = &wins[focused_win];
        if (w->active && !w->minimized) {
            /* Close button */
            int cx = w->x+14, cy2 = w->y+TITLEBAR_H/2;
            if (abs(x-cx)<8 && abs(y-cy2)<8) { close_window(focused_win); return; }
            /* Min button */
            cx = w->x+30;
            if (abs(x-cx)<8 && abs(y-cy2)<8) { w->minimized=1; return; }
            /* Max button */
            cx = w->x+46;
            if (abs(x-cx)<8 && abs(y-cy2)<8) {
                if (!w->maximized) {
                    w->maximized=1;
                    w->x=0; w->y=0; w->w=fb_w; w->h=fb_h-TASKBAR_H;
                } else {
                    w->maximized=0;
                    w->x=80; w->y=60; w->w=480; w->h=320;
                }
                return;
            }
            /* Zewpol launch button */
            if (w->app_id == ZEWPOL_IDX) {
                int bx = w->x+2 + (w->w-4)/2-50;
                int by = w->y+TITLEBAR_H+2 + (w->h-TITLEBAR_H-4)-36;
                if (x>=bx && x<=bx+100 && y>=by && y<=by+22) {
                    pid_t pid = fork();
                    if (pid==0) {
                        execl("/bin/sh","sh","-c",
                              "links https://zewpol.neocities.org",NULL);
                        exit(0);
                    }
                    return;
                }
            }
        }
    }

    /* Taskbar window buttons */
    if (y >= fb_h-TASKBAR_H) {
        int bx = 66;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!wins[i].active) continue;
            if (x>=bx && x<=bx+100) {
                if (wins[i].minimized) wins[i].minimized=0;
                focused_win = i;
                return;
            }
            bx += 108;
        }
        return;
    }

    /* Focus window under cursor */
    for (int i = MAX_WINDOWS-1; i >= 0; i--) {
        if (!wins[i].active || wins[i].minimized) continue;
        Window *w = &wins[i];
        if (x>=w->x && x<=w->x+w->w && y>=w->y && y<=w->y+w->h) {
            focused_win = i;
            /* Start drag if titlebar */
            if (y <= w->y+TITLEBAR_H) {
                w->drag=1; w->drag_ox=x-w->x; w->drag_oy=y-w->y;
            }
            return;
        }
    }

    /* Desktop icon click */
    int cols = ICON_COLS;
    int start_x = fb_w - cols*(ICON_SIZE+ICON_PAD) - 10;
    int start_y = 16;
    for (int i = 0; i < APP_COUNT; i++) {
        int col = i % cols;
        int row = i / cols;
        int ix = start_x + col*(ICON_SIZE+ICON_PAD);
        int iy = start_y + row*(ICON_SIZE+ICON_PAD+16);
        if (x>=ix && x<=ix+ICON_SIZE && y>=iy && y<=iy+ICON_SIZE) {
            launch_app(i);
            return;
        }
    }
}

static void handle_mouse_release(void) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        wins[i].drag = 0;
}

static void handle_mouse_move(int x, int y) {
    mx = x; my = y;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wins[i].drag && wins[i].active) {
            wins[i].x = x - wins[i].drag_ox;
            wins[i].y = y - wins[i].drag_oy;
        }
    }
}

static void handle_key(int keycode, int value) {
    if (!value) return;
    if (focused_win < 0) return;
    Window *w = &wins[focused_win];
    if (!w->active) return;

    /* Text input for editor/notes/terminal */
    if (w->app_id == 2 || w->app_id == 11 || w->app_id == 0) {
        char *buf = w->edit_buf;
        int  *pos = &w->edit_pos;
        if (keycode == 14 && *pos > 0) { /* backspace */
            buf[--(*pos)] = '\0';
        } else if (keycode >= 16 && keycode <= 50) {
            /* Basic keycode->char mapping (US QWERTY) */
            const char kmap[] = "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0zxcvbnm";
            int idx = keycode - 16;
            if (idx < (int)sizeof(kmap) && kmap[idx] && *pos < 4090) {
                buf[(*pos)++] = kmap[idx];
                buf[*pos] = '\0';
            }
        } else if (keycode == 57 && *pos < 4090) { /* space */
            buf[(*pos)++] = ' '; buf[*pos] = '\0';
        } else if (keycode == 28 && *pos < 4090) { /* enter */
            buf[(*pos)++] = '\n'; buf[*pos] = '\0';
        }
    }

    /* Close window on Alt+F4 (keycode 62 = F4, needs alt tracking) */
    if (keycode == 1) close_window(focused_win); /* ESC closes */
}

/* ═══════════════════════════════════════════════════════════
   FRAMEBUFFER INIT
   ═══════════════════════════════════════════════════════════ */

static int fb_init(void) {
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        fprintf(stderr, "ztkgui: cannot open /dev/fb0: %s\n", strerror(errno));
        return -1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);

    fb_w  = vinfo.xres;
    fb_h  = vinfo.yres;
    fb_sz = fb_w * fb_h * 4;

    fb = mmap(NULL, fb_sz, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb == MAP_FAILED) {
        fprintf(stderr, "ztkgui: mmap failed\n");
        return -1;
    }

    bb = malloc(fb_sz);
    if (!bb) { fprintf(stderr, "ztkgui: out of memory\n"); return -1; }

    printf("ztkgui: framebuffer %dx%d @ 32bpp\n", fb_w, fb_h);
    return 0;
}

static void fb_cleanup(void) {
    if (bb) free(bb);
    if (fb && fb != MAP_FAILED) munmap(fb, fb_sz);
    if (fb_fd >= 0) close(fb_fd);
}

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ═══════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    if (fb_init() < 0) return 1;

    /* Open mouse & keyboard input */
    int mouse_fd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
    int kbd_fd   = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);

    mx = fb_w / 2;
    my = fb_h / 2;

    printf("ztkgui: ZTK Desktop Environment starting...\n");
    printf("ztkgui: %d apps registered (%d web apps)\n",
           APP_COUNT, 1);
    printf("ztkgui: Zewpol pinned at index %d -> %s\n",
           ZEWPOL_IDX, apps[ZEWPOL_IDX].url);

    /* Main render loop */
    while (running) {
        /* Mouse input */
        if (mouse_fd >= 0) {
            unsigned char mb[3];
            if (read(mouse_fd, mb, 3) == 3) {
                int dx = (int8_t)mb[1];
                int dy = (int8_t)mb[2];
                mx = mx + dx;
                my = my - dy;
                if (mx < 0) mx = 0;
                if (my < 0) my = 0;
                if (mx >= fb_w) mx = fb_w-1;
                if (my >= fb_h-1) my = fb_h-1;

                int new_btn = mb[0] & 1;
                if (new_btn && !mbtn)   handle_click(mx, my);
                if (!new_btn && mbtn)   handle_mouse_release();
                if (new_btn)            handle_mouse_move(mx, my);
                mbtn = new_btn;
            }
        }

        /* Keyboard input */
        if (kbd_fd >= 0) {
            struct input_event ev;
            if (read(kbd_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY)
                    handle_key(ev.code, ev.value);
            }
        }

        /* Render frame */
        draw_desktop();
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (wins[i].active && !wins[i].minimized && i != focused_win)
                draw_window(i);
        if (focused_win >= 0 && wins[focused_win].active)
            draw_window(focused_win);
        draw_taskbar();
        draw_cursor();
        flip_buffer();

        usleep(16667); /* ~60 fps */
    }

    fb_cleanup();
    if (mouse_fd >= 0) close(mouse_fd);
    if (kbd_fd   >= 0) close(kbd_fd);
    return 0;
}
