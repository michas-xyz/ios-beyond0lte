/* SPDX-License-Identifier: BSD-2-Clause
 * Boot menu screen. Laid out for the S10 portrait panel (1440 x 3040); every pixel write is clipped, so another
 * size only crops. shortcut: the layout does not scale down below ~1300 px wide, add a scale step if another
 * board needs it.
 * Font: Spleen 32x64 and 16x32 by Frederic Cambus, BSD-2-Clause (notice in FontSpleen*.h).
 */
#include "BootMenuDraw.h"
#include "BootMenu.h"
#include "FontSpleen16x32.h"
#include "FontSpleen32x64.h"

#define BG       0x0f1115u
#define CARD     0x1c1f26u
#define ACCENT   0x0a84ffu
#define TEXT     0xf2f2f7u
#define DIM      0x8e8e93u
#define DIM_SEL  0xd6e8ffu
#define AMBER    0xff9f0au
#define TRACK    0x2c2f36u

struct canvas { uint32_t *px, stride, w, h; };
struct font { const unsigned char *bits; unsigned w, h; };

static const struct font big = { &spleen_32_64[0][0], SPLEEN_32_64_W, SPLEEN_32_64_H };
static const struct font small = { &spleen_16_32[0][0], SPLEEN_16_32_W, SPLEEN_16_32_H };

static const char *const labels[B2L_MENU_COUNT] = { "Boot iOS", "Reboot to recovery", "Reboot" };
static const char *const details[B2L_MENU_COUNT] = {
    "Start iOS 26 now", "Restart into TWRP", "Restart the phone",
};

static void put(const struct canvas *c, int x, int y, uint32_t color)
{
    if (x >= 0 && y >= 0 && (uint32_t)x < c->w && (uint32_t)y < c->h) c->px[(uint32_t)y * c->stride + (uint32_t)x] = color;
}

static uint32_t mix(uint32_t a, uint32_t b, unsigned n, unsigned d)   /* a + (b - a) * n / d, per channel */
{
    uint32_t out = 0;
    for (unsigned s = 0; s < 24; s += 8) {
        unsigned ca = (a >> s) & 0xff, cb = (b >> s) & 0xff;
        out |= ((ca * (d - n) + cb * n) / d) << s;
    }
    return out;
}

/* Rounded rectangle over a plain colour (under); corners antialiased by 4x4 supersampling. */
static void round_rect(const struct canvas *c, int x, int y, int w, int h, int r, uint32_t color, uint32_t under)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            /* corner circle centre in continuous coordinates, 0 when the pixel is outside the corner squares */
            int cx = i < r ? r : i >= w - r ? w - r : 0, cy = j < r ? r : j >= h - r ? h - r : 0;
            if (!cx || !cy) { put(c, x + i, y + j, color); continue; }
            unsigned hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int dx = 8 * (i - cx) + 2 * sx + 1, dy = 8 * (j - cy) + 2 * sy + 1;   /* 1/8 pixel units */
                    hits += dx * dx + dy * dy <= 64 * r * r;
                }
            if (hits) put(c, x + i, y + j, mix(under, color, hits, 16));
        }
}

static void text(const struct canvas *c, const struct font *f, unsigned scale, int x, int y, const char *s, uint32_t color)
{
    unsigned bpr = (f->w + 7) / 8;
    for (; *s; s++, x += (int)(f->w * scale)) {
        unsigned ch = (unsigned char)*s;
        const unsigned char *g = f->bits + (ch >= 0x20 && ch <= 0x7e ? ch - 0x20 : '?' - 0x20) * f->h * bpr;
        for (unsigned row = 0; row < f->h; row++)
            for (unsigned col = 0; col < f->w; col++)
                if (g[row * bpr + col / 8] & (0x80 >> (col % 8)))
                    for (unsigned k = 0; k < scale * scale; k++)
                        put(c, x + (int)(col * scale + k % scale), y + (int)(row * scale + k / scale), color);
    }
}

static unsigned len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

static void centered(const struct canvas *c, const struct font *f, unsigned scale, int y, const char *s, uint32_t color)
{
    text(c, f, scale, ((int)c->w - (int)(len(s) * f->w * scale)) / 2, y, s, color);
}

static void number(char *out, const char *pre, unsigned n, const char *post)
{
    char digits[12]; unsigned k = 0;
    do digits[k++] = (char)('0' + n % 10); while ((n /= 10) && k < sizeof(digits));
    while (*pre) *out++ = *pre++;
    while (k) *out++ = digits[--k];
    while (*post) *out++ = *post++;
    *out = 0;
}

/* Word-wraps a note into lines of at most cols characters. */
static void note_lines(const struct canvas *c, int x, int y, unsigned cols, const char *s)
{
    char line[64];
    if (cols >= sizeof(line)) cols = sizeof(line) - 1;
    while (*s) {
        unsigned n = 0, cut = 0;
        while (s[n] && n < cols) { if (s[n] == ' ') cut = n; n++; }
        if (s[n] && cut) n = cut;
        for (unsigned i = 0; i < n; i++) line[i] = s[i];
        line[n] = 0;
        text(c, &big, 1, x, y, line, AMBER);
        y += (int)big.h + 12;
        s += n;
        while (*s == ' ') s++;
    }
}

void b2l_menu_render(uint32_t *pixels, uint32_t stride, uint32_t width, uint32_t height, const struct b2l_menu_view *v)
{
    const struct canvas c = { pixels, stride, width, height };
    const int margin = 96, cw = (int)width - 2 * margin, ch = 208, gap = 36, H = (int)height;
    char line[64];

    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) pixels[y * stride + x] = BG;

    int y = H * 13 / 100;
    centered(&c, &big, 2, y, "iOS 26", TEXT);
    y += (int)big.h * 2 + 24;
    centered(&c, &big, 1, y, "Galaxy S10 - XNU loader", DIM);

    y = H * 29 / 100;
    for (unsigned i = 0; i < B2L_MENU_COUNT; i++, y += ch + gap) {
        int sel = i == v->selected;
        round_rect(&c, margin, y, cw, ch, 40, sel ? ACCENT : CARD, BG);
        text(&c, &big, 1, margin + 64, y + 36, labels[i], TEXT);
        text(&c, &small, 1, margin + 64, y + 36 + (int)big.h + 16, details[i], sel ? DIM_SEL : DIM);
        if (sel) text(&c, &big, 1, margin + cw - 64 - (int)big.w, y + (ch - (int)big.h) / 2, ">", TEXT);
    }

    y += 60;
    if (v->status) {
        centered(&c, &big, 1, y, v->status, TEXT);
    } else if (v->seconds_left) {
        number(line, "Booting iOS in ", v->seconds_left, v->seconds_left == 1 ? " second" : " seconds");
        centered(&c, &big, 1, y, line, TEXT);
        int bar_y = y + (int)big.h + 40, full = cw - 2 * 64;
        unsigned t = v->timeout_s ? v->timeout_s : 1;
        round_rect(&c, margin + 64, bar_y, full, 16, 8, TRACK, BG);
        int fill = (int)((uint64_t)full * (v->seconds_left > t ? t : v->seconds_left) / t);
        if (fill >= 16) round_rect(&c, margin + 64, bar_y, fill, 16, 8, ACCENT, TRACK);
    } else {
        centered(&c, &big, 1, y, "Automatic boot is off", DIM);
    }

    if (v->note) note_lines(&c, margin, H * 70 / 100, (unsigned)cw / big.w, v->note);

    static const char *const keys[2][2] = { { "Volume +/-", "move" }, { "Power", "select" } };
    int kx = margin + 64;
    y = H - 120 - 2 * ((int)big.h + 24);
    for (unsigned i = 0; i < 2; i++, y += (int)big.h + 24) {
        text(&c, &big, 1, kx, y, keys[i][0], TEXT);
        text(&c, &big, 1, kx + 14 * (int)big.w, y, keys[i][1], DIM);
    }
}
