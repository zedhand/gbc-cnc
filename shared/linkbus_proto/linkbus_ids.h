/*
 * LinkBus v1 — message types and module IDs.
 * 0x00-0x0F : common to every module      0x10-0x3F : GBA -> module (module-specific)
 * 0x40-0x7F : module -> GBA (module-specific)
 * All multi-byte fields little-endian. Strings are NUL-terminated and truncated to fit.
 */
#ifndef LINKBUS_IDS_H
#define LINKBUS_IDS_H

/* ---- module IDs -------------------------------------------------------- */
#define LB_MOD_CNC_PENDANT      0x0001u
#define LB_MOD_WARDRIVER        0x0002u
#define LB_MOD_EXPERIMENTAL     0x7F00u   /* 0x7F00-0x7FFF free for bench hacks */

/* ---- common (all modules) --------------------------------------------- */
#define LB_HELLO        0x01  /* M->G  u8 proto, u16 module_id, u16 fw_ver, char name[]          */
#define LB_HELLO_REQ    0x02  /* G->M  (empty) ask module to resend HELLO                        */
#define LB_LOG          0x03  /* M->G  u8 level, char text[]   level: 0 info, 1 warn, 2 error    */
#define LB_PING         0x04  /* either way, echo payload back as PONG                          */
#define LB_PONG         0x05

/* ---- generic lists (files, macros, wifi APs ...) ----------------------- */
#define LB_LIST_REQ     0x06  /* G->M  u8 kind                                                   */
#define LB_LIST_ENTRY   0x07  /* M->G  u8 kind, u8 index, u8 total, u32 arg, char name[]         */
#define LB_LIST_END     0x08  /* M->G  u8 kind, u8 total                                         */
#define LB_LIST_RUN     0x09  /* G->M  u8 kind, u8 index                                         */

/* ======================================================================== */
/*  CNC pendant (module 0x0001)                                             */
/* ======================================================================== */

#define CNC_LIST_FILES   0     /* LIST_ENTRY.arg = file size in bytes                          */
#define CNC_LIST_MACROS  1     /* LIST_ENTRY.arg bit0 = needs confirm                           */

/* GBA -> module */
#define CNC_RT          0x10  /* u8 realtime byte ('!', '~', 0x18, 0x85, override bytes)        */
#define CNC_LINE        0x11  /* char line[]  (refused while a job runs)                        */
#define CNC_JOG_STEP    0x12  /* u8 axis(0 X,1 Y,2 Z), i32 dist_um, u16 feed_mm_min             */
#define CNC_JOG_CONT    0x13  /* i8 dx, i8 dy, i8 dz (-1/0/1), u16 feed. Resend every 50 ms     */
                              /* while held; module cancels jog after 150 ms of silence.        */
#define CNC_JOG_STOP    0x14  /* (empty) jog cancel now                                         */
#define CNC_JOB_STOP    0x15  /* (empty) feed hold -> wait for Hold:0 -> soft reset             */

/* GRBL realtime override bytes (sent with CNC_RT) */
#define GRBL_OV_FEED_RESET   0x90
#define GRBL_OV_FEED_UP10    0x91
#define GRBL_OV_FEED_DOWN10  0x92
#define GRBL_OV_SPIN_RESET   0x99
#define GRBL_OV_SPIN_UP10    0x9A
#define GRBL_OV_SPIN_DOWN10  0x9B

/* module -> GBA */
#define CNC_STATUS      0x40  /* see cnc_status layout below                                    */
#define CNC_JOB_EVENT   0x41  /* u8 event, u16 arg                                              */

/* CNC_STATUS payload (34 bytes):
 *   0  u8  state      (CNC_ST_*)
 *   1  u8  sub        (Hold:n / Door:n sub-state, else 0)
 *   2  i32 mpos_x_um   6 i32 mpos_y_um  10 i32 mpos_z_um
 *  14  i32 wpos_x_um  18 i32 wpos_y_um  22 i32 wpos_z_um
 *  26  u16 feed (mm/min)
 *  28  u16 spindle (rpm / S value)
 *  30  u8  flags      (CNC_FL_*)
 *  31  u8  reserved
 *  32  u16 progress   (0..1000, permille of current job)
 *  34  u8  feed override %   35 u8 rapid override %   36 u8 spindle override %   (added in fw 1.1)
 * Receivers must accept >= CNC_STATUS_MIN bytes and treat the override bytes as 100 if absent.
 */
#define CNC_STATUS_MIN  34
#define CNC_STATUS_LEN  37

enum {
    CNC_ST_OFFLINE = 0,   /* no answer from GRBL */
    CNC_ST_IDLE, CNC_ST_RUN, CNC_ST_HOLD, CNC_ST_JOG, CNC_ST_ALARM,
    CNC_ST_DOOR, CNC_ST_CHECK, CNC_ST_HOME, CNC_ST_SLEEP
};

#define CNC_FL_JOB_ACTIVE  0x01
#define CNC_FL_STOPPING    0x02
#define CNC_FL_WIFI        0x04

enum {
    CNC_EV_STARTED = 1, CNC_EV_DONE, CNC_EV_ABORTED, CNC_EV_ERROR /* arg = grbl error code */,
    CNC_EV_ALARM /* arg = alarm code */, CNC_EV_REFUSED /* command refused: busy/not idle */
};

#endif
