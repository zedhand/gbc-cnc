#include "gfx.h"

void gfx_init(void)
{
#ifndef HOST_BUILD
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;
#endif
}

void gfx_rect(int x, int y, int w, int h, u16 c)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    for (j = 0; j < h; j++) {
        volatile u16 *row = &VRAM[(y + j) * SCREEN_W + x];
        for (i = 0; i < w; i++) row[i] = c;
    }
}

void gfx_hline(int x, int y, int w, u16 c) { gfx_rect(x, y, w, 1, c); }

void gfx_frame(int x, int y, int w, int h, u16 c)
{
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c);
    gfx_rect(x + w - 1, y, 1, h, c);
}

static void glyph(int x, int y, unsigned char ch, u16 fg, u16 bg, int s)
{
    const u8 *g;
    int r, c, sy, sx;
    if (ch < 0x20 || ch > 0x7F) ch = '?';
    g = font8x8[ch - 0x20];
    if (x < 0 || y < 0 || x + 8 * s > SCREEN_W || y + 8 * s > SCREEN_H) return;
    for (r = 0; r < 8; r++) {
        u8 bits = g[r];
        for (sy = 0; sy < s; sy++) {
            volatile u16 *row = &VRAM[(y + r * s + sy) * SCREEN_W + x];
            for (c = 0; c < 8; c++) {
                u16 px = (bits >> c) & 1 ? fg : bg;
                for (sx = 0; sx < s; sx++) *row++ = px;
            }
        }
    }
}

int gfx_text(int x, int y, const char *s, u16 fg, u16 bg, int scale)
{
    while (*s) { glyph(x, y, (unsigned char)*s++, fg, bg, scale); x += 8 * scale; }
    return x;
}

int gfx_text_w(int x, int y, const char *s, int cells, u16 fg, u16 bg, int scale)
{
    int i;
    for (i = 0; i < cells; i++) {
        glyph(x, y, *s ? (unsigned char)*s : ' ', fg, bg, scale);
        if (*s) s++;
        x += 8 * scale;
    }
    return x;
}

void gfx_text_r(int x_right, int y, const char *s, int cells, u16 fg, u16 bg, int scale)
{
    int len = 0, pad, i;
    while (s[len]) len++;
    if (len > cells) { s += len - cells; len = cells; }
    pad = cells - len;
    x_right -= cells * 8 * scale;
    for (i = 0; i < pad; i++) { glyph(x_right, y, ' ', fg, bg, scale); x_right += 8 * scale; }
    gfx_text(x_right, y, s, fg, bg, scale);
}

/* ---- formatting ---- */

void fmt_uint(char *out, u32 v)
{
    char tmp[11];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *out++ = tmp[--n];
    *out = 0;
}

void fmt_um(char *out, s32 um)
{
    u32 a = um < 0 ? (u32)(-um) : (u32)um;
    u32 frac = a % 1000;
    if (um < 0) *out++ = '-';
    fmt_uint(out, a / 1000);
    while (*out) out++;
    *out++ = '.';
    *out++ = (char)('0' + frac / 100);
    *out++ = (char)('0' + frac / 10 % 10);
    *out++ = (char)('0' + frac % 10);
    *out = 0;
}

void fmt_pad(char *out, const char *a, int width)
{
    int i = 0;
    while (a[i] && i < width) { out[i] = a[i]; i++; }
    while (i < width) out[i++] = ' ';
    out[i] = 0;
}
