// =====================================================================================
//  LinkBus CNC pendant module — configuration
//  Target: ESP-12F (ESP8266, 4 MB). Edit, then `pio run -t upload` (first time over USB,
//  afterwards `pio run -e ota -t upload`).
// =====================================================================================
#pragma once

// ---- identity ---------------------------------------------------------------------
#define MODULE_NAME        "CNC Pendant"
#define MODULE_FW_VERSION  0x0101          // 1.1: overrides in STATUS
#define HOSTNAME           "linkbus-cnc"   // mDNS / OTA: linkbus-cnc.local

// ---- Wi-Fi ------------------------------------------------------------------------
// Leave WIFI_SSID empty to always run as an access point.
#define WIFI_SSID          ""
#define WIFI_PASS          ""
#define AP_SSID            "LinkBus-CNC"
#define AP_PASS            "linkbus123"    // >= 8 chars
#define OTA_PASS           "linkbus"

// ---- GBA link port (via coupler) ----------------------------------------------------
#define PIN_LINK_SC        14   // -> GBA SC  (link pin 5)   220R series
#define PIN_LINK_SI        13   // -> GBA SI  (link pin 3)   220R series
#define PIN_LINK_SO        12   // <- GBA SO  (link pin 2)   220R series
#define ROM_PATH           "/rom/pendant.gba"

// ---- GRBL ------------------------------------------------------------------------
// UART0 (GPIO1 TX -> GRBL RX, GPIO3 RX <- GRBL TX via 5V->3.3V divider).
// Debug log goes to Serial1 (GPIO2, TX only).
#define GRBL_BAUD          115200
#define GRBL_RX_BUFFER     128   // GRBL 1.1 on ATmega328P. Raise for 32-bit GRBL ports.
#define GRBL_POLL_IDLE_MS  200
#define GRBL_POLL_FAST_MS  100   // while running / jogging

// ---- jogging ---------------------------------------------------------------------
#define JOG_INVERT_X       0     // set to 1 if D-pad Right moves the spindle left
#define JOG_INVERT_Y       0     // set to 1 if D-pad Up moves the gantry toward you
#define JOG_INVERT_Z       0
#define JOG_MAX_FEED_XY    2500  // mm/min, clamps whatever the GBA asks for
#define JOG_MAX_FEED_Z     600
#define JOG_CONT_SEG_MS    100   // continuous jog: each segment = this much travel time
#define JOG_CONT_LEAD      2     // segments queued ahead of the machine
#define JOG_DEADMAN_MS     150   // no JOG_CONT heartbeat for this long -> jog cancel

// ---- probing ---------------------------------------------------------------------
// Thickness of whatever the probe touches. 0 = clip on the PCB copper itself.
#define PROBE_THICKNESS    "0.000"
#define PROBE_FEED         "60"
#define PROBE_DEPTH        "15"     // max probe travel, mm

// ---- macros ----------------------------------------------------------------------
// name shown on the GBA, G-code lines separated by '\n', confirm = ask "A to confirm".
// Allowed in Idle or Alarm state, never during a job.
struct Macro { const char *name; const char *gcode; bool confirm; };

static const Macro MACROS[] = {
    { "Unlock ($X)",        "$X",                                   false },
    { "Home ($H)",          "$H",                                   true  },
    { "Zero X+Y",           "G10 L20 P0 X0 Y0",                     false },
    { "Zero Z",             "G10 L20 P0 Z0",                        false },
    { "Zero X+Y+Z",         "G10 L20 P0 X0 Y0 Z0",                  false },
    { "Go to X0 Y0 (+5 Z)", "G91 G0 Z5\nG90 G0 X0 Y0",              true  },
    { "Probe Z",            "G91 G38.2 Z-" PROBE_DEPTH " F" PROBE_FEED "\n"
                            "G10 L20 P0 Z" PROBE_THICKNESS "\n"
                            "G91 G0 Z3\nG90",                       true  },
    { "Spindle off (M5)",   "M5",                                   false },
    { "Check mode ($C)",    "$C",                                   true  },
};
static const uint8_t MACRO_COUNT = sizeof(MACROS) / sizeof(MACROS[0]);

#define GCODE_DIR          "/gcode"
#define MAX_FILES          64
