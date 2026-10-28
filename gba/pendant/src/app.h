#ifndef APP_H
#define APP_H
#include "gba.h"

enum { SCR_JOG, SCR_FILES, SCR_MACROS, SCR_INFO, SCR_COUNT };

#define MAX_FILES   64
#define MAX_MACROS  16
#define NAME_LEN    28

typedef struct { char name[NAME_LEN]; u32 arg; } list_item_t;

typedef struct {
    /* link */
    u8   linked;            /* transfers arriving from a module */
    u8   module_ok;         /* HELLO received from a CNC pendant module */
    u8   wrong_module;      /* HELLO from some other module type */
    char module_name[24];
    u16  module_fw;
    u16  crc_errors;
    u16  words_per_s;
    u32  packets_rx, status_rx;

    /* machine status (from CNC_STATUS) */
    u8   have_status;
    u8   state, sub, flags;
    s32  mpos[3], wpos[3];
    u16  feed, spindle, progress;
    u8   ov_feed, ov_rapid, ov_spindle;   /* percent */

    /* UI */
    u8   screen;
    u8   step_idx, feed_idx;
    list_item_t files[MAX_FILES];
    u8   file_count, files_loading, files_requested;
    list_item_t macros[MAX_MACROS];
    u8   macro_count, macros_loading, macros_requested;
    u8   cursor[SCR_COUNT], scroll[SCR_COUNT];
    u8   dialog, dialog_kind, dialog_index;
    char msg[40];
    u8   msg_level;
    u16  msg_timer;
    char job_name[NAME_LEN];
    u8   jogging;
    u32  frame;
    u8   dirty, dirty_full;
} app_t;

extern app_t app;

/* jog tables */
#define STEP_COUNT 5
#define FEED_COUNT 5
extern const s32  step_um[STEP_COUNT];   /* 0 = continuous */
extern const char *const step_label[STEP_COUNT];
extern const u16  feed_mm_min[FEED_COUNT];

#define LIST_ROWS 9

const char *state_name(u8 st);
void app_message(u8 level, const char *text);
void ui_draw(void);

#endif
