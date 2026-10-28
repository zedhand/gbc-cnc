/* Pendant screens. Pure drawing from `app` state — also builds on the host for screenshots. */
#include "app.h"
#include "gfx.h"
#include "linkbus_ids.h"

#define C_BG      RGB15(2, 3, 4)
#define C_PANEL   RGB15(5, 6, 8)
#define C_SEL     RGB15(7, 9, 13)
#define C_FG      RGB15(29, 29, 30)
#define C_MUTED   RGB15(15, 16, 18)
#define C_ACCENT  RGB15(14, 24, 31)
#define C_OK      RGB15(10, 27, 12)
#define C_WARN    RGB15(31, 23, 5)
#define C_BAD     RGB15(31, 9, 9)
#define C_BLACK   RGB15(1, 1, 2)

#define CONTENT_Y 28
#define FOOTER_Y  141

static const char *const tab_name[SCR_COUNT] = { "JOG", "FILES", "MACROS", "INFO" };

const char *state_name(u8 st)
{
    static const char *const n[] = { "OFFLINE", "IDLE", "RUN", "HOLD", "JOG", "ALARM",
                                     "DOOR", "CHECK", "HOME", "SLEEP" };
    return st < 10 ? n[st] : "?";
}

static u16 state_color(u8 st)
{
    switch (st) {
    case CNC_ST_IDLE:  return C_OK;
    case CNC_ST_RUN:   case CNC_ST_JOG: case CNC_ST_HOME: return C_ACCENT;
    case CNC_ST_HOLD:  case CNC_ST_DOOR: case CNC_ST_CHECK: return C_WARN;
    case CNC_ST_ALARM: return C_BAD;
    default:           return C_MUTED;
    }
}

/* ---- header / footer ------------------------------------------------------------------ */

static void draw_header(void)
{
    const char *pill;
    u16 pc;
    gfx_text_w(4, 3, app.module_ok ? app.module_name : "LinkBus", 20, C_FG, C_PANEL, 1);

    if (!app.linked)                    { pill = "NO LINK"; pc = C_MUTED; }
    else if (!app.module_ok)            { pill = app.wrong_module ? "WRONG" : "HELLO?"; pc = C_MUTED; }
    else if (app.flags & CNC_FL_STOPPING) { pill = "STOP.."; pc = C_BAD; }
    else                                { pill = state_name(app.state); pc = state_color(app.state); }

    gfx_rect(170, 1, 68, 12, pc);
    {   /* centre the pill text */
        int len = 0;
        while (pill[len]) len++;
        gfx_text(170 + (68 - len * 8) / 2, 3, pill, C_BLACK, pc, 1);
    }
}

static void draw_tabs(void)
{
    int i, x = 4;
    gfx_rect(0, 14, SCREEN_W, 12, C_BG);
    for (i = 0; i < SCR_COUNT; i++) {
        u16 fg = i == app.screen ? C_FG : C_MUTED;
        int x2 = gfx_text(x, 16, tab_name[i], fg, C_BG, 1);
        if (i == app.screen) gfx_hline(x, 25, x2 - x, C_ACCENT);
        x = x2 + 16;
    }
}

static const char *hint_text(void)
{
    /* max 29 characters */
    if (app.dialog) return "A:confirm  B:cancel";
    if ((app.flags & CNC_FL_JOB_ACTIVE) && app.screen == SCR_JOG) return "^v feed <> spindle A:100%";
    if (app.flags & CNC_FL_JOB_ACTIVE) return "SEL:hold/go  hold SEL:STOP";
    if (app.screen == SCR_INFO) return "START:tab";
    switch (app.screen) {
    case SCR_JOG:
        if (app.state == CNC_ST_ALARM) return "ALARM - MACROS > Unlock";
        return "A:step B:feed START:tab";
    case SCR_FILES:  return "A:run B:reload START:tab";
    default:         return "A:run  START:tab";
    }
}

static void draw_footer(void)
{
    u16 mc = app.msg_level == 2 ? C_BAD : app.msg_level == 1 ? C_WARN : C_FG;
    gfx_text_w(2, FOOTER_Y + 1, app.msg_timer ? app.msg : "", 29, mc, C_PANEL, 1);
    gfx_text_w(2, FOOTER_Y + 10, hint_text(), 29, C_MUTED, C_PANEL, 1);
}

/* ---- screens ---------------------------------------------------------------------------- */

static void draw_progress(int y)
{
    char pct[8];
    int w = 186, fill, n = 0;
    if (!(app.flags & CNC_FL_JOB_ACTIVE)) { gfx_rect(0, y, SCREEN_W, 10, C_BG); return; }
    fill = (int)((u32)app.progress * (u32)w / 1000u);
    if (fill > w) fill = w;
    gfx_frame(4, y, w + 2, 9, C_MUTED);
    gfx_rect(5, y + 1, fill, 7, C_ACCENT);
    gfx_rect(5 + fill, y + 1, w - fill, 7, C_BG);
    fmt_uint(pct, app.progress / 10);
    while (pct[n]) n++;
    pct[n] = '%'; pct[n + 1] = 0;
    gfx_text_r(236, y, pct, 4, C_FG, C_BG, 1);
}

static void draw_jog(void)
{
    static const char ax[3] = { 'X', 'Y', 'Z' };
    char buf[16], l[2] = { 0, 0 };
    int i;

    if (!app.module_ok) {
        gfx_text_w(8, 50, app.linked ? "Module found, waiting" : "Waiting for module...", 26, C_FG, C_BG, 1);
        gfx_text_w(8, 64, app.linked ? "for HELLO." : "Check the link cable.", 26, C_MUTED, C_BG, 1);
        if (app.wrong_module) {
            gfx_text_w(8, 82, "Not a CNC module:", 26, C_WARN, C_BG, 1);
            gfx_text_w(8, 92, app.module_name, 26, C_WARN, C_BG, 1);
        }
        return;
    }

    for (i = 0; i < 3; i++) {
        int y = CONTENT_Y + 2 + i * 22;
        l[0] = ax[i];
        gfx_text(6, y, l, C_ACCENT, C_BG, 2);
        fmt_um(buf, app.wpos[i]);
        gfx_text_r(156, y, app.have_status ? buf : "---", 8, C_FG, C_BG, 2);
        gfx_text(162, y + 1, "M", C_MUTED, C_BG, 1);
        fmt_um(buf, app.mpos[i]);
        gfx_text_r(236, y + 1, app.have_status ? buf : "", 8, C_MUTED, C_BG, 1);
    }

    if (app.flags & CNC_FL_JOB_ACTIVE) {
        /* job running: show feed / spindle overrides instead of jog settings */
        int x, y = CONTENT_Y + 70;
        x = gfx_text(6, y, "FEED ", C_MUTED, C_BG, 1);
        fmt_uint(buf, app.ov_feed);
        x = gfx_text(x, y, buf, app.ov_feed == 100 ? C_FG : C_WARN, C_BG, 1);
        x = gfx_text(x, y, "%  ", C_FG, C_BG, 1);
        x = gfx_text(x, y, "SPINDLE ", C_MUTED, C_BG, 1);
        fmt_uint(buf, app.ov_spindle);
        x = gfx_text(x, y, buf, app.ov_spindle == 100 ? C_FG : C_WARN, C_BG, 1);
        x = gfx_text(x, y, "%", C_FG, C_BG, 1);
        if (x < 236) gfx_rect(x, y, 236 - x, 8, C_BG);
    } else {
        int x, y = CONTENT_Y + 70;
        char line[32];
        x = gfx_text(6, y, "STEP ", C_MUTED, C_BG, 1);
        fmt_pad(line, step_label[app.step_idx], 5);
        x = gfx_text(x, y, line, app.step_idx ? C_FG : C_ACCENT, C_BG, 1);
        x = gfx_text(x + 8, y, "FEED ", C_MUTED, C_BG, 1);
        fmt_uint(buf, feed_mm_min[app.feed_idx]);
        fmt_pad(line, buf, 5);
        x = gfx_text(x, y, line, C_FG, C_BG, 1);
        if (x < 236) gfx_rect(x, y, 236 - x, 8, C_BG);
    }
    {
        char line[32];
        int x;
        x = gfx_text(6, CONTENT_Y + 82, "F ", C_MUTED, C_BG, 1);
        fmt_uint(buf, app.feed);
        fmt_pad(line, buf, 6);
        x = gfx_text(x, CONTENT_Y + 82, line, C_FG, C_BG, 1);
        x = gfx_text(x, CONTENT_Y + 82, "S ", C_MUTED, C_BG, 1);
        fmt_uint(buf, app.spindle);
        fmt_pad(line, buf, 6);
        x = gfx_text(x, CONTENT_Y + 82, line, C_FG, C_BG, 1);
        gfx_text_w(x, CONTENT_Y + 82, app.jogging ? "JOG" : "", 3, C_ACCENT, C_BG, 1);
    }
    draw_progress(CONTENT_Y + 98);
}

static void draw_list(const list_item_t *items, u8 count, u8 loading, u8 kind)
{
    u8 *cur = &app.cursor[app.screen], *top = &app.scroll[app.screen];
    int r;
    if (*cur >= count && count) *cur = count - 1;
    if (*cur < *top) *top = *cur;
    if (*cur >= *top + LIST_ROWS) *top = (u8)(*cur - LIST_ROWS + 1);

    for (r = 0; r < LIST_ROWS; r++) {
        int idx = *top + r, y = CONTENT_Y + 2 + r * 10;
        u16 bg = (idx == *cur && idx < count) ? C_SEL : C_BG;
        gfx_rect(0, y - 1, SCREEN_W, 10, bg);
        if (idx >= count) {
            if (r == 0) gfx_text(16, y, loading ? "Loading..." : "(empty)", C_MUTED, bg, 1);
            continue;
        }
        if (idx == *cur) gfx_text(4, y, "\x7F", C_ACCENT, bg, 1);
        if (kind == CNC_LIST_FILES) {
            char kb[12];
            u32 k = (items[idx].arg + 1023) / 1024;
            gfx_text_w(16, y, items[idx].name, 21, C_FG, bg, 1);
            fmt_uint(kb, k);
            { int n = 0; while (kb[n]) n++; kb[n] = 'K'; kb[n + 1] = 0; }
            gfx_text_r(236, y, kb, 5, C_MUTED, bg, 1);
        } else {
            gfx_text_w(16, y, items[idx].name, 24, C_FG, bg, 1);
            if (items[idx].arg & 1) gfx_text(228, y, "!", C_WARN, bg, 1);
        }
    }
    if (kind == CNC_LIST_FILES) draw_progress(CONTENT_Y + 98);
}

static void draw_dialog(void)
{
    const list_item_t *it = app.dialog_kind == CNC_LIST_FILES ? &app.files[app.dialog_index]
                                                             : &app.macros[app.dialog_index];
    gfx_rect(16, 46, 208, 58, C_PANEL);
    gfx_frame(16, 46, 208, 58, C_ACCENT);
    gfx_text(24, 54, app.dialog_kind == CNC_LIST_FILES ? "Run this file?" : "Run this macro?", C_FG, C_PANEL, 1);
    gfx_text_w(24, 68, it->name, 24, C_ACCENT, C_PANEL, 1);
    gfx_text(24, 88, "A: run      B: cancel", C_MUTED, C_PANEL, 1);
}

static void info_row(int row, const char *label, const char *value, u16 vc)
{
    int y = CONTENT_Y + 4 + row * 12;
    gfx_text_w(8, y, label, 10, C_MUTED, C_BG, 1);
    gfx_text_w(96, y, value, 17, vc, C_BG, 1);
}

static void draw_info(void)
{
    char b[24], n[12];
    int i;
    info_row(0, "Module", app.module_ok ? app.module_name : (app.linked ? "(no HELLO)" : "(none)"), C_FG);

    b[0] = 0;
    if (app.module_ok) {
        fmt_uint(b, app.module_fw >> 8);
        i = 0; while (b[i]) i++;
        b[i++] = '.';
        fmt_uint(b + i, app.module_fw & 0xFF);
    }
    info_row(1, "Firmware", b, C_FG);

    fmt_uint(b, app.words_per_s);
    i = 0; while (b[i]) i++;
    { const char *s = " words/s"; while (*s) b[i++] = *s++; b[i] = 0; }
    info_row(2, "Link", app.linked ? b : "down", app.linked ? C_OK : C_BAD);

    fmt_uint(b, app.crc_errors);
    info_row(3, "CRC errors", b, app.crc_errors ? C_WARN : C_FG);

    fmt_uint(b, app.packets_rx);
    info_row(4, "Packets", b, C_FG);
    fmt_uint(b, app.status_rx);
    info_row(5, "Status", b, C_FG);

    /* overrides: "F100 R100 S100" */
    {
        const u8 v[3] = { app.ov_feed, app.ov_rapid, app.ov_spindle };
        const char tag[3] = { 'F', 'R', 'S' };
        int k = 0, j;
        for (j = 0; j < 3; j++) {
            b[k++] = tag[j];
            fmt_uint(n, v[j]);
            for (i = 0; n[i]; i++) b[k++] = n[i];
            if (j < 2) b[k++] = ' ';
        }
        b[k] = 0;
    }
    info_row(6, "Overrides", b, C_FG);
    info_row(7, "Protocol", "LinkBus v1", C_MUTED);
}

/* ---- entry ------------------------------------------------------------------------------- */

void ui_draw(void)
{
    if (app.dirty_full) {
        gfx_rect(0, 0, SCREEN_W, 14, C_PANEL);
        gfx_rect(0, 14, SCREEN_W, FOOTER_Y - 14, C_BG);
        gfx_rect(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, C_PANEL);
        draw_tabs();
        app.dirty_full = 0;
    }
    draw_header();
    if (app.dialog) {
        draw_dialog();
    } else {
        switch (app.screen) {
        case SCR_JOG:    draw_jog(); break;
        case SCR_FILES:  draw_list(app.files, app.file_count, app.files_loading, CNC_LIST_FILES); break;
        case SCR_MACROS: draw_list(app.macros, app.macro_count, app.macros_loading, CNC_LIST_MACROS); break;
        case SCR_INFO:   draw_info(); break;
        }
    }
    draw_footer();
    app.dirty = 0;
}
