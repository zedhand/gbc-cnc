/* Pendant logic: packets in, buttons in, LinkBus commands out. No hardware access here
 * except through lb_send(), so it also runs in the host test harness. */
#include "app.h"
#include "lb_gba.h"

app_t app;

const s32  step_um[STEP_COUNT]          = { 0, 10, 100, 1000, 10000 };
const char *const step_label[STEP_COUNT] = { "CONT", "0.01", "0.1", "1", "10" };
const u16  feed_mm_min[FEED_COUNT]      = { 100, 300, 600, 1000, 2000 };

#define HOLD_TO_STOP  45   /* frames (0.75 s) holding SELECT = stop */
#define REPEAT_DELAY  18
#define REPEAT_RATE   6
#define JOG_BEAT      3    /* frames between JOG_CONT heartbeats (50 ms) */

static void copy_str(char *dst, const char *src, int cap)
{
    int i;
    for (i = 0; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

void app_message(u8 level, const char *text)
{
    copy_str(app.msg, text, sizeof(app.msg));
    app.msg_level = level;
    app.msg_timer = 300;      /* 5 s */
    app.dirty = 1;
}

static void send_rt(u8 b)                { lb_send(CNC_RT, &b, 1); }
static void request_list(u8 kind)
{
    lb_send(LB_LIST_REQ, &kind, 1);
    if (kind == CNC_LIST_FILES) { app.files_loading = 1; app.file_count = 0; app.files_requested = 1; }
    else                        { app.macros_loading = 1; app.macro_count = 0; app.macros_requested = 1; }
    app.dirty = 1;
}

/* ---- packets ---------------------------------------------------------------------------- */

void app_packet(u8 type, const u8 *p, u8 len)
{
    app.packets_rx++;
    if (app.screen == SCR_INFO) app.dirty = 1;
    switch (type) {
    case LB_HELLO: {
        const lb_module_info_t *m = lb_module();
        u8 was = app.module_ok;
        copy_str(app.module_name, m->name, sizeof(app.module_name));
        app.module_fw = m->fw_version;
        app.module_ok = m->module_id == LB_MOD_CNC_PENDANT;
        app.wrong_module = !app.module_ok;
        if (app.module_ok && !was) {
            app.files_requested = app.macros_requested = 0;
            request_list(CNC_LIST_MACROS);
            app.dirty_full = 1;
        }
        app.dirty = 1;
        break;
    }
    case CNC_STATUS:
        if (len < CNC_STATUS_MIN) break;
        {
            int i;
            app.status_rx++;
            app.state = p[0];
            app.sub = p[1];
            for (i = 0; i < 3; i++) {
                app.mpos[i] = (s32)lb_get32(p + 2 + 4 * i);
                app.wpos[i] = (s32)lb_get32(p + 14 + 4 * i);
            }
            app.feed = lb_get16(p + 26);
            app.spindle = lb_get16(p + 28);
            app.flags = p[30];
            app.progress = lb_get16(p + 32);
            if (len >= 37) { app.ov_feed = p[34]; app.ov_rapid = p[35]; app.ov_spindle = p[36]; }
            else           { app.ov_feed = app.ov_rapid = app.ov_spindle = 100; }
            app.have_status = 1;
            app.dirty = 1;
        }
        break;

    case LB_LOG:
        if (len >= 2) {
            char t[40];
            copy_str(t, (const char *)p + 1, (len - 1) < (int)sizeof(t) ? (len - 1) + 1 : (int)sizeof(t));
            app_message(p[0], t);
        }
        break;

    case LB_LIST_ENTRY:
        if (len >= 8) {
            u8 kind = p[0], idx = p[1];
            list_item_t *it = 0;
            if (kind == CNC_LIST_FILES && idx < MAX_FILES) {
                it = &app.files[idx];
                if (idx >= app.file_count) app.file_count = idx + 1;
            } else if (kind == CNC_LIST_MACROS && idx < MAX_MACROS) {
                it = &app.macros[idx];
                if (idx >= app.macro_count) app.macro_count = idx + 1;
            }
            if (it) {
                it->arg = lb_get32(p + 3);
                copy_str(it->name, (const char *)p + 7, (len - 7) < NAME_LEN ? (len - 7) + 1 : NAME_LEN);
                app.dirty = 1;
            }
        }
        break;

    case LB_LIST_END:
        if (len >= 2) {
            u8 total = p[1];
            if (p[0] == CNC_LIST_FILES) { app.files_loading = 0; if (total < app.file_count) app.file_count = total; }
            else                        { app.macros_loading = 0; if (total < app.macro_count) app.macro_count = total; }
            app.dirty = 1;
        }
        break;

    case CNC_JOB_EVENT:
        if (len >= 3) {
            u16 arg = lb_get16(p + 1);
            switch (p[0]) {
            case CNC_EV_STARTED: app_message(0, "Job started"); break;
            case CNC_EV_DONE:    app_message(0, "Job done"); break;
            case CNC_EV_ABORTED: app_message(1, "Job stopped"); break;
            case CNC_EV_REFUSED: app_message(1, "Refused: machine busy"); break;
            case CNC_EV_ERROR:   /* the module sends the detailed LOG line */ break;
            case CNC_EV_ALARM:   if (!arg) app_message(2, "ALARM"); break;
            }
        }
        break;
    }
}

/* ---- link watchdog ---------------------------------------------------------------------- */

void app_link_tick(u32 words_rx, u16 crc_errors)
{
    static u32 last_words;
    static u32 last_change;
    if (words_rx != last_words) {
        last_words = words_rx;
        last_change = app.frame;
        if (!app.linked) {
            app.linked = 1;
            lb_send(LB_HELLO_REQ, 0, 0);
            app.dirty_full = app.dirty = 1;
        }
    } else if (app.linked && app.frame - last_change > 30) {
        app.linked = 0;
        app.module_ok = app.wrong_module = 0;
        app.have_status = 0;
        app.file_count = app.macro_count = 0;
        app.dialog = 0;
        app.jogging = 0;
        lb_forget_module();
        app.dirty_full = app.dirty = 1;
    }
    if (crc_errors != app.crc_errors) { app.crc_errors = crc_errors; app.dirty = 1; }
    {
        static u32 win_start, win_words;
        if (app.frame - win_start >= 60) {
            app.words_per_s = (u16)(words_rx - win_words);
            win_words = words_rx;
            win_start = app.frame;
            if (app.screen == SCR_INFO) app.dirty = 1;
        }
    }
}

/* ---- input -------------------------------------------------------------------------------- */

static u16 held_frames[10];

static int repeat(u16 keys, u16 pressed, u16 key, int bit)
{
    if (pressed & key) { held_frames[bit] = 0; return 1; }
    if (keys & key) {
        u16 h = ++held_frames[bit];
        return h >= REPEAT_DELAY && ((h - REPEAT_DELAY) % REPEAT_RATE) == 0;
    }
    return 0;
}

static int machine_moving(void)
{
    return app.state == CNC_ST_RUN || app.state == CNC_ST_JOG || app.state == CNC_ST_HOME;
}

/* During a job the D-pad trims the cut: Up/Down feed +-10 %, Right/Left spindle +-10 %,
 * A resets both to 100 %. Jogging is impossible then anyway. */
static void override_input(u16 keys, u16 pressed)
{
    if (repeat(keys, pressed, KEY_UP, 4))    send_rt(GRBL_OV_FEED_UP10);
    if (repeat(keys, pressed, KEY_DOWN, 5))  send_rt(GRBL_OV_FEED_DOWN10);
    if (repeat(keys, pressed, KEY_RIGHT, 6)) send_rt(GRBL_OV_SPIN_UP10);
    if (repeat(keys, pressed, KEY_LEFT, 7))  send_rt(GRBL_OV_SPIN_DOWN10);
    if (pressed & KEY_A) { send_rt(GRBL_OV_FEED_RESET); send_rt(GRBL_OV_SPIN_RESET); app_message(0, "Overrides 100%"); }
}

static void jog_input(u16 keys, u16 pressed)
{
    if (app.flags & CNC_FL_JOB_ACTIVE) { override_input(keys, pressed); return; }

    if (step_um[app.step_idx] == 0) {
        /* continuous: heartbeat while any direction is held, explicit stop on release */
        static s8 last[3];
        static u32 last_tx;
        s8 d[3];
        d[0] = (keys & KEY_RIGHT) ? 1 : (keys & KEY_LEFT) ? -1 : 0;
        d[1] = (keys & KEY_UP)    ? 1 : (keys & KEY_DOWN) ? -1 : 0;
        d[2] = (keys & KEY_R)     ? 1 : (keys & KEY_L)    ? -1 : 0;
        if (d[0] || d[1] || d[2]) {
            int changed = d[0] != last[0] || d[1] != last[1] || d[2] != last[2];
            if (changed || app.frame - last_tx >= JOG_BEAT) {
                u8 b[5] = { (u8)d[0], (u8)d[1], (u8)d[2], 0, 0 };
                lb_put16(b + 3, feed_mm_min[app.feed_idx]);
                lb_send(CNC_JOG_CONT, b, 5);
                last_tx = app.frame;
                last[0] = d[0]; last[1] = d[1]; last[2] = d[2];
            }
            if (!app.jogging) { app.jogging = 1; app.dirty = 1; }
        } else if (app.jogging) {
            lb_send(CNC_JOG_STOP, 0, 0);
            app.jogging = 0;
            last[0] = last[1] = last[2] = 0;
            app.dirty = 1;
        }
        return;
    }

    /* step mode: one move per press, auto-repeat while held */
    {
        static const u16 key[6]  = { KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN, KEY_R, KEY_L };
        static const u8  axis[6] = { 0, 0, 1, 1, 2, 2 };
        static const s8  sign[6] = { 1, -1, 1, -1, 1, -1 };
        int i;
        for (i = 0; i < 6; i++) {
            if (repeat(keys, pressed, key[i], 4 + i)) {
                u8 b[7];
                b[0] = axis[i];
                lb_put32(b + 1, (u32)(step_um[app.step_idx] * sign[i]));
                lb_put16(b + 5, feed_mm_min[app.feed_idx]);
                lb_send(CNC_JOG_STEP, b, 7);
            }
        }
    }
}

static void stop_jogging(void)
{
    if (app.jogging) { lb_send(CNC_JOG_STOP, 0, 0); app.jogging = 0; }
}

void app_input(u16 keys)
{
    static u16 prev;
    static u32 sel_t0;
    static u8  sel_done;
    u16 pressed = keys & ~prev, released = prev & ~keys;
    prev = keys;

    if (!app.module_ok) return;

    /* SELECT: press = feed hold if moving; tap while held = resume; hold = STOP */
    if (pressed & KEY_SELECT) {
        sel_t0 = app.frame;
        sel_done = 0;
        if (machine_moving()) { stop_jogging(); send_rt('!'); sel_done = 1; }
    }
    if ((keys & KEY_SELECT) && !(sel_done & 2) && app.frame - sel_t0 >= HOLD_TO_STOP) {
        stop_jogging();
        lb_send(CNC_JOB_STOP, 0, 0);
        app_message(2, "STOP");
        sel_done |= 2;
    }
    if ((released & KEY_SELECT) && !sel_done && app.state == CNC_ST_HOLD) {
        send_rt('~');
        app_message(0, "Resume");
    }

    /* dialog has the buttons to itself */
    if (app.dialog) {
        if (pressed & KEY_A) {
            u8 b[2] = { app.dialog_kind, app.dialog_index };
            lb_send(LB_LIST_RUN, b, 2);
            if (app.dialog_kind == CNC_LIST_FILES)
                copy_str(app.job_name, app.files[app.dialog_index].name, NAME_LEN);
            app.dialog = 0;
            app.dirty_full = app.dirty = 1;
        } else if (pressed & KEY_B) {
            app.dialog = 0;
            app.dirty_full = app.dirty = 1;
        }
        return;
    }

    if (pressed & KEY_START) {
        stop_jogging();
        app.screen = (u8)((app.screen + 1) % SCR_COUNT);
        if (app.screen == SCR_FILES && !app.files_requested) request_list(CNC_LIST_FILES);
        if (app.screen == SCR_MACROS && !app.macros_requested) request_list(CNC_LIST_MACROS);
        app.dirty_full = app.dirty = 1;
        return;
    }

    switch (app.screen) {
    case SCR_JOG:
        if (app.flags & CNC_FL_JOB_ACTIVE) { jog_input(keys, pressed); break; }
        if (pressed & KEY_A) { stop_jogging(); app.step_idx = (u8)((app.step_idx + 1) % STEP_COUNT); app.dirty = 1; }
        if (pressed & KEY_B) { app.feed_idx = (u8)((app.feed_idx + 1) % FEED_COUNT); app.dirty = 1; }
        jog_input(keys, pressed);
        break;

    case SCR_FILES:
    case SCR_MACROS: {
        u8 count = app.screen == SCR_FILES ? app.file_count : app.macro_count;
        u8 *cur = &app.cursor[app.screen];
        if (repeat(keys, pressed, KEY_UP, 0) && *cur > 0) { (*cur)--; app.dirty = 1; }
        if (repeat(keys, pressed, KEY_DOWN, 1) && *cur + 1 < count) { (*cur)++; app.dirty = 1; }
        if (repeat(keys, pressed, KEY_LEFT, 2)) { *cur = *cur > LIST_ROWS ? (u8)(*cur - LIST_ROWS) : 0; app.dirty = 1; }
        if (repeat(keys, pressed, KEY_RIGHT, 3) && count) {
            *cur = (u8)((*cur + LIST_ROWS < count) ? *cur + LIST_ROWS : count - 1); app.dirty = 1;
        }
        if ((pressed & KEY_B) && app.screen == SCR_FILES) request_list(CNC_LIST_FILES);
        if ((pressed & KEY_A) && *cur < count) {
            if (app.screen == SCR_FILES) {
                if (app.flags & CNC_FL_JOB_ACTIVE) { app_message(1, "A job is already running"); break; }
                app.dialog_kind = CNC_LIST_FILES;
                app.dialog = 1;
            } else if (app.macros[*cur].arg & 1) {
                app.dialog_kind = CNC_LIST_MACROS;
                app.dialog = 1;
            } else {
                u8 b[2] = { CNC_LIST_MACROS, *cur };
                lb_send(LB_LIST_RUN, b, 2);
            }
            app.dialog_index = *cur;
            app.dirty_full = app.dirty = 1;
        }
        break;
    }
    }
}

void app_tick(void)
{
    if (app.msg_timer && --app.msg_timer == 0) app.dirty = 1;
}

void app_init(void)
{
    app.step_idx = 3;        /* 1 mm */
    app.feed_idx = 3;        /* 1000 mm/min */
    app.ov_feed = app.ov_rapid = app.ov_spindle = 100;
    app.dirty = app.dirty_full = 1;
}
