/* Mode 3 drawing: rectangles and opaque 8x8 text (scale 1 or 2). Text cells are drawn with
 * their background, so values can be overwritten in place every frame without flicker. */
#ifndef GFX_H
#define GFX_H
#include "gba.h"

#ifdef HOST_BUILD
extern u16 host_vram[SCREEN_W * SCREEN_H];
#define VRAM host_vram
#else
#define VRAM ((volatile u16 *)0x06000000)
#endif

extern const u8 font8x8[96][8];

void gfx_init(void);                                  /* mode 3, BG2 on */
void gfx_rect(int x, int y, int w, int h, u16 c);
void gfx_hline(int x, int y, int w, u16 c);
void gfx_frame(int x, int y, int w, int h, u16 c);    /* 1 px outline */
/* draw text, returns x after the last glyph. scale: 1 or 2 */
int  gfx_text(int x, int y, const char *s, u16 fg, u16 bg, int scale);
/* text padded with background to exactly `cells` glyphs (truncated if longer) */
int  gfx_text_w(int x, int y, const char *s, int cells, u16 fg, u16 bg, int scale);
/* right-aligned in a field of `cells` glyphs ending at x_right */
void gfx_text_r(int x_right, int y, const char *s, int cells, u16 fg, u16 bg, int scale);

/* number formatting (no printf) */
void fmt_um(char *out, s32 um);            /* micrometres -> "-12.345" */
void fmt_uint(char *out, u32 v);
void fmt_pad(char *out, const char *a, int width);

#endif
