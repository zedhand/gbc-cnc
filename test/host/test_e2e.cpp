// End-to-end host simulation:
//   real GBA pendant code (app.c, ui.c, lb_gba.c)  <-- bit-level simulated link port -->
//   real ESP module code (LinkMaster, Grbl, CncModule)  <-- serial -->  fake GRBL 1.1
// The GBA starts in the multiboot BIOS; the module uploads the real compiled ROM image.
#include "Arduino.h"
#include "LittleFS.h"
#include "LinkMaster.h"
#include "Grbl.h"
#include "CncModule.h"
#include <deque>
#include <vector>
#include <cmath>

extern "C" {
#include "gba.h"
#include "lb_gba.h"
#include "app.h"
#include "gfx.h"
void app_init(void);
void app_packet(u8 type, const u8 *p, u8 len);
void app_input(u16 keys);
void app_link_tick(u32 words_rx, u16 crc_errors);
void app_tick(void);
volatile u16 hreg[0x210];
volatile u32 hreg_siodata32;
volatile u16 hreg_bios_if;
void (*volatile hreg_isr)(void);
u16 host_vram[SCREEN_W * SCREEN_H];
void vblank_wait(void) {}
}

LogSerial Serial1;
fs::FS LittleFS;

// =========================================================================================
// time & failures
// =========================================================================================
static uint64_t sim_us = 0;
uint32_t micros() { return (uint32_t)sim_us; }
uint32_t millis() { return (uint32_t)(sim_us / 1000); }
void delayMicroseconds(uint32_t us) { sim_us += us; }
void delay(uint32_t ms) { sim_us += (uint64_t)ms * 1000; }
void yield() {}

static int failures = 0;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); failures++; } printf("\n"); } while (0)

// =========================================================================================
// simulated GBA link port (slave)
// =========================================================================================
enum GbaMode { GBA_OFF, GBA_BIOS, GBA_ROM };
static GbaMode gba_mode = GBA_BIOS;
static bool cable = true;
static int pin_sc = 1, pin_si = 1, pin_so = 1, bitn = 0;
static uint32_t txsh, rxsh;
static bool xfer_active;
static uint64_t isr_due = 0;
static bool isr_pending = false;
static uint32_t isr_latency_max = 0;     // random ISR latency, us
static uint32_t corrupt_every = 0;       // corrupt 1 in N completed words
static uint32_t words_done = 0, words_unarmed = 0;

// --- BIOS multiboot model (lagged replies like real SPI) ---
static uint32_t bios_reply = 0, bios_st = 0, b_seed, b_crcA, b_crcC, b_idx, b_fsize, b_waits;
static const uint32_t b_crcB = 0x5A;
static std::vector<uint8_t> bios_img;
static bool bios_boot_ok = false;
static uint32_t crc_step(uint32_t crc, uint32_t d) { for (int b = 0; b < 32; b++) { uint32_t bit = (crc ^ d) & 1; crc = (crc >> 1) ^ (bit ? 0xC37B : 0); d >>= 1; } return crc; }
static void gba_boot_rom();
static void bios_step(uint32_t w)
{
    switch (bios_st) {
    case 0: if (w == 0x6202) { bios_reply = 0x72020000; bios_st = 1; } break;
    case 1: if (w == 0x6102) { bios_st = 2; b_idx = 0; bios_img.assign(0x40000, 0); } break;
    case 2: bios_img[b_idx] = w & 0xFF; bios_img[b_idx + 1] = (w >> 8) & 0xFF; b_idx += 2;
            bios_reply = ((0xC0 - b_idx) / 2) << 16; if (b_idx == 0xC0) bios_st = 3; break;
    case 3: if (w == 0x6200) bios_st = 4; break;
    case 4: if (w == 0x6202) bios_st = 5; break;
    case 5: if (w == 0x63D1) { bios_reply = 0x73CC0000; bios_st = 6; } break;
    case 6: if ((w >> 8) == 0x64) { b_crcA = w & 0xFF; b_seed = 0xFFFF00D1 | (0xCC << 8); bios_reply = 0x73000000 | (b_crcB << 16); bios_st = 7; } break;
    case 7: b_fsize = w * 4 + 0x190; b_idx = 0xC0; b_crcC = 0xC387; bios_st = 8; bios_reply = 0; break;
    case 8: { b_seed = b_seed * 0x6F646573u + 1; uint32_t d = w ^ (0xFE000000u - b_idx) ^ b_seed ^ 0x43202F2Fu;
              memcpy(&bios_img[b_idx], &d, 4); b_crcC = crc_step(b_crcC, d); b_idx += 4; if (b_idx >= b_fsize) bios_st = 9; } break;
    case 9: if (w == 0x65) bios_reply = (++b_waits > 2) ? 0x00750000 : 0x00740000; else if (w == 0x66) bios_st = 10; break;
    case 10: bios_boot_ok = (w == (crc_step(b_crcC, 0xFFFF0000u | (b_crcB << 8) | b_crcA) & 0xFFFF));
             bios_st = 11; gba_mode = GBA_ROM; gba_boot_rom(); break;
    }
}

static void run_isr()
{
    isr_pending = false;
    u16 f = hreg[(0x04000202 - 0x04000000) / 2] & hreg[(0x04000200 - 0x04000000) / 2];
    if (f & IRQ_SERIAL) lb_irq();
    hreg[(0x04000202 - 0x04000000) / 2] &= (u16)~f;
}

static void sc_fall()
{
    if (bitn == 0) {
        if (isr_pending && sim_us >= isr_due) run_isr();
        rxsh = 0;
        if (!cable || gba_mode == GBA_OFF) { xfer_active = false; txsh = 0xFFFFFFFF; }
        else if (gba_mode == GBA_BIOS)     { xfer_active = true;  txsh = bios_reply; }
        else { xfer_active = (REG_SIOCNT & SIO_START) != 0; txsh = xfer_active ? REG_SIODATA32 : 0xFFFFFFFF; }
    }
    pin_so = (txsh >> 31) & 1;
    txsh <<= 1;
}

static void sc_rise()
{
    rxsh = (rxsh << 1) | (uint32_t)pin_si;
    if (++bitn < 32) return;
    bitn = 0;
    if (!xfer_active) { words_unarmed++; return; }
    words_done++;
    if (gba_mode == GBA_BIOS) { bios_step(rxsh); return; }
    if (corrupt_every && (rxsh >> 24) != 0xA0 && (rxsh >> 24) != 0) {   // data-carrying words only
        static uint32_t data_words; if (++data_words % corrupt_every == 0) rxsh ^= 1u << (data_words % 24);
    }
    REG_SIODATA32 = rxsh;
    REG_SIOCNT &= (u16)~SIO_START;
    REG_IF |= IRQ_SERIAL;
    isr_pending = true;
    isr_due = sim_us + (isr_latency_max ? (uint64_t)(rand() % isr_latency_max) : 0);
}

void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t pin, uint8_t v)
{
    if (pin == PIN_LINK_SC) {
        if (pin_sc && !v) sc_fall();
        if (!pin_sc && v) sc_rise();
        pin_sc = v;
    } else if (pin == PIN_LINK_SI) pin_si = v;
}
int digitalRead(uint8_t pin) { return pin == PIN_LINK_SO ? (cable ? pin_so : 1) : 1; }

// =========================================================================================
// GBA "CPU": boot + one frame of main.c's loop
// =========================================================================================
static u16 keys = 0;
static void gba_boot_rom()
{
    memset((void *)hreg, 0, sizeof(hreg));
    memset(&app, 0, sizeof(app));
    gfx_init();
    app_init();
    lb_init();
    REG_IME = 1;
}
static void gba_frame()
{
    if (isr_pending && sim_us >= isr_due) run_isr();
    u8 type, len; const u8 *pl;
    app.frame++;
    while (lb_poll(&type, &pl, &len)) app_packet(type, pl, len);
    app_link_tick(lb_words_rx(), lb_crc_errors());
    app_input(keys);
    app_tick();
    if (app.dirty || app.dirty_full) ui_draw();
}

// =========================================================================================
// fake GRBL 1.1 (ATmega328P: 128-byte RX buffer, 15-block planner)
// =========================================================================================
struct Block { int32_t from[3], to[3]; uint64_t dur, done; bool jog; };
class FakeGrbl : public Stream {
public:
    std::deque<uint8_t> rx, tx;          // rx: ESP->GRBL serial buffer, tx: GRBL->ESP
    std::deque<Block> planner;
    int32_t pos[3] = {0, 0, 0}, wco[3] = {0, 0, 0};
    enum { IDLE, RUN, HOLD, JOG, ALARM } st = IDLE;
    bool hold = false; uint64_t hold_t = 0;
    bool abs_mode = true; uint32_t feed = 500;
    size_t max_rx = 0; int lines_ok = 0, job_lines = 0, motion_lines = 0, jog_lines = 0, resets = 0;
    int alarms = 0, holds = 0, resumes = 0, jog_cancels = 0;
    int ov_feed = 100, ov_spindle = 100;
    double max_planned_s = 0;

    size_t write(uint8_t b) override {
        switch (b) {
        case '?': report(); return 1;
        case '!': if (!planner.empty() && !hold) { hold = true; hold_t = sim_us; holds++; } return 1;
        case '~': if (hold) { hold = false; resumes++; } return 1;
        case 0x85: jog_cancel(); return 1;
        case 0x18: reset(); return 1;
        }
        if (b == 0x90) { ov_feed = 100; return 1; }
        if (b == 0x91) { ov_feed = std::min(200, ov_feed + 10); return 1; }
        if (b == 0x92) { ov_feed = std::max(10, ov_feed - 10); return 1; }
        if (b == 0x99) { ov_spindle = 100; return 1; }
        if (b == 0x9A) { ov_spindle = std::min(200, ov_spindle + 10); return 1; }
        if (b == 0x9B) { ov_spindle = std::max(10, ov_spindle - 10); return 1; }
        if (b >= 0x80) return 1;
        rx.push_back(b);
        max_rx = std::max(max_rx, rx.size());
        return 1;
    }
    int available() override { return (int)tx.size(); }
    int read() override { if (tx.empty()) return -1; int c = tx.front(); tx.pop_front(); return c; }
    void out(const std::string &s) { for (char c : s) tx.push_back((uint8_t)c); tx.push_back('\r'); tx.push_back('\n'); }

    const char *state_str() {
        if (st == ALARM) return "Alarm";
        if (hold) return (sim_us - hold_t < 40000) ? "Hold:1" : "Hold:0";
        if (planner.empty()) return "Idle";
        return planner.front().jog ? "Jog" : "Run";
    }
    int32_t cur(int i) {
        if (planner.empty()) return pos[i];
        const Block &b = planner.front();
        return b.from[i] + (int32_t)((int64_t)(b.to[i] - b.from[i]) * (int64_t)b.done / (int64_t)std::max<uint64_t>(b.dur, 1));
    }
    void report() {
        char s[160];
        snprintf(s, sizeof s, "<%s|MPos:%.3f,%.3f,%.3f|FS:%u,0|WCO:%.3f,%.3f,%.3f|Ov:%d,100,%d>", state_str(),
                 cur(0) / 1000.0, cur(1) / 1000.0, cur(2) / 1000.0, planner.empty() ? 0 : feed * ov_feed / 100,
                 wco[0] / 1000.0, wco[1] / 1000.0, wco[2] / 1000.0, ov_feed, ov_spindle);
        out(s);
    }
    void jog_cancel() {
        bool any = false;
        if (!planner.empty() && planner.front().jog) {
            int32_t here[3] = { cur(0), cur(1), cur(2) };
            memcpy(pos, here, sizeof pos);
            while (!planner.empty() && planner.front().jog) planner.pop_front();
            any = true;
        }
        // harsh model: unparsed $J lines in the RX buffer are dropped WITHOUT an ok
        std::string all(rx.begin(), rx.end()), keep, line;
        for (char c : all) { line += c; if (c == '\n') { if (line.rfind("$J=", 0) != 0) keep += line; line.clear(); } }
        keep += line;
        rx.assign(keep.begin(), keep.end());
        hold = false;
        if (any) jog_cancels++;
    }
    void reset() {
        bool moving = !planner.empty() && !(hold && sim_us - hold_t >= 40000);
        if (!planner.empty()) { int32_t h[3] = { cur(0), cur(1), cur(2) }; memcpy(pos, h, sizeof pos); }
        planner.clear(); rx.clear(); hold = false; resets++;
        out("");
        out("Grbl 1.1h ['$' for help]");
        if (moving) { st = ALARM; alarms++; out("ALARM:3"); }
    }
    double planned_seconds() { double s = 0; for (auto &b : planner) s += (b.dur - b.done) / 1e6; return s; }
    bool parse_axes(const char *l, int32_t t[3], bool rel, bool *any) {
        *any = false;
        for (int i = 0; i < 3; i++) {
            const char *p = strchr(l, "XYZ"[i]);
            if (p) { double v = atof(p + 1); int32_t um = (int32_t)llround(v * 1000); t[i] = rel ? t[i] + um : um - (int32_t)0 + wco[i]; *any = true; }
        }
        const char *f = strchr(l, 'F'); if (f) feed = (uint32_t)atoi(f + 1);
        return true;
    }
    void add_motion(int32_t to[3], bool jog, bool rapid) {
        int32_t from[3];
        if (planner.empty()) memcpy(from, pos, sizeof from); else memcpy(from, planner.back().to, sizeof from);
        double d = 0; for (int i = 0; i < 3; i++) d += pow((to[i] - from[i]) / 1000.0, 2); d = sqrt(d);
        double f = rapid ? 3000 : std::max<uint32_t>(feed, 1);
        Block b; memcpy(b.from, from, sizeof from); memcpy(b.to, to, sizeof b.to);
        b.dur = (uint64_t)(d / f * 60e6) + 1000; b.done = 0; b.jog = jog;
        planner.push_back(b);
        max_planned_s = std::max(max_planned_s, planned_seconds());
    }
    void exec_line(const std::string &l) {
        if (l.empty()) return;
        if (l.rfind("$J=", 0) == 0) {
            if (st == ALARM) { out("error:9"); return; }
            int32_t base[3]; if (planner.empty()) memcpy(base, pos, sizeof base); else memcpy(base, planner.back().to, sizeof base);
            bool rel = l.find("G91") != std::string::npos, any;
            int32_t t[3] = { base[0], base[1], base[2] };
            if (!rel) memcpy(t, base, sizeof t);
            parse_axes(l.c_str() + 3, t, rel, &any);
            if (t[0] > 300000 || t[0] < -300000) { out("error:15"); return; }
            add_motion(t, true, false); jog_lines++; out("ok"); return;
        }
        if (l == "$X") { if (st == ALARM) { st = IDLE; out("[MSG:Caution: Unlocked]"); } out("ok"); return; }
        if (l == "$H") { st = IDLE; memset(pos, 0, sizeof pos); out("ok"); return; }
        if (l.rfind("$", 0) == 0) { out("ok"); return; }
        if (st == ALARM) { out("error:9"); return; }
        if (l == "BAD") { out("error:20"); return; }
        if (l.find("G10") != std::string::npos && l.find("L20") != std::string::npos) {
            for (int i = 0; i < 3; i++) { size_t p = l.find("XYZ"[i], 3); if (p != std::string::npos) wco[i] = pos[i] - (int32_t)llround(atof(l.c_str() + p + 1) * 1000); }
            out("ok"); return;
        }
        if (l.find("G38.2") != std::string::npos) { pos[2] -= 2000; out("[PRB:0.000,0.000,-2.000:1]"); out("ok"); return; }
        if (l.find("G91") != std::string::npos) abs_mode = false;
        if (l.find("G90") != std::string::npos) abs_mode = true;
        bool rapid = l.rfind("G0", 0) == 0 && (l.size() < 3 || !isdigit((unsigned char)l[2]));
        int32_t t[3]; if (planner.empty()) memcpy(t, pos, sizeof t); else memcpy(t, planner.back().to, sizeof t);
        int32_t base[3]; memcpy(base, t, sizeof base);
        bool any = false;
        for (int i = 0; i < 3; i++) {
            size_t p = l.find("XYZ"[i]);
            if (p == std::string::npos) continue;
            double v = atof(l.c_str() + p + 1); int32_t um = (int32_t)llround(v * 1000);
            t[i] = abs_mode ? um + wco[i] : base[i] + um; any = true;
        }
        size_t fp = l.find('F'); if (fp != std::string::npos) feed = (uint32_t)atoi(l.c_str() + fp + 1);
        if (any) { add_motion(t, false, rapid); motion_lines++; }
        lines_ok++;
        out("ok");
    }
    void tick(uint64_t dt) {
        // parse complete lines while the planner has room
        while (planner.size() < 15) {
            auto it = std::find(rx.begin(), rx.end(), '\n');
            if (it == rx.end()) break;
            std::string l(rx.begin(), it);
            rx.erase(rx.begin(), it + 1);
            exec_line(l);
        }
        if (hold || planner.empty()) return;
        Block &b = planner.front();
        b.done += b.jog ? dt : dt * (uint64_t)ov_feed / 100;
        if (b.done >= b.dur) { memcpy(pos, b.to, sizeof pos); planner.pop_front(); }
    }
};

static FakeGrbl fake;
static LinkMaster gLink;
static Grbl grbl;
static CncModule cnc;

// =========================================================================================
// simulation driver
// =========================================================================================
static uint64_t next_frame = 0;
static std::vector<std::string> esp_log;
static void logline(const char *m) { esp_log.push_back(m); }

static void step_for(double seconds)
{
    uint64_t end = sim_us + (uint64_t)(seconds * 1e6);
    while (sim_us < end) {
        uint64_t t0 = sim_us;
        gLink.loop();
        grbl.loop();
        cnc.loop();
        sim_us += 20;
        fake.tick(sim_us - t0);
        if (isr_pending && sim_us >= isr_due) run_isr();
        if (sim_us >= next_frame) {
            next_frame = sim_us + 16743;
            if (gba_mode == GBA_ROM) gba_frame();
        }
    }
}
static void press(u16 k, double hold_s = 0.05) { keys |= k; step_for(hold_s); keys &= (u16)~k; step_for(0.05); }
static void goto_screen(int s) { for (int i = 0; i < 4 && app.screen != s; i++) press(KEY_START); }

static void screenshot(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s.rgb15", getenv("SHOTS") ? getenv("SHOTS") : ".", name);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(host_vram, 2, sizeof(host_vram) / 2, f); fclose(f); }
}

static std::string read_file(const char *p)
{
    FILE *f = fopen(p, "rb"); std::string s;
    if (!f) return s;
    char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f); return s;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s pendant_rom.bin\n", argv[0]); return 2; }
    srand(7);

    // --- filesystem: ROM + jobs ---
    LittleFS.mem.files[ROM_PATH] = read_file(argv[1]);
    std::string job = "( LinkBus test job )\n%\nG21 G90\nG0 Z5\nF800\n";
    int job_moves = 0;
    for (int i = 0; i < 300; i++) {
        char l[64];
        snprintf(l, sizeof l, "G1 X%.3f Y%.3f ; segment %d\n", 10 + 5 * sin(i / 9.0), 10 + 5 * cos(i / 9.0), i);
        job += l; job_moves++;
    }
    job += "G0 Z5\nM5\n";
    LittleFS.mem.files["/gcode/circle_test.nc"] = job;
    LittleFS.mem.files["/gcode/pcb_isolation_top.nc"] = "G21\nG90\nG0 Z2\nG1 X1 F300\n";
    LittleFS.mem.files["/gcode/too_long_line.nc"] = "G21\nG1 X1 " + std::string(100, '0') + "1\n";
    LittleFS.mem.files["/gcode/bad_command.nc"] = "G21\nG0 X1\nBAD\nG0 X2\nG0 X3\n";
    printf("ROM image %zu bytes\n", LittleFS.mem.files[ROM_PATH].size());

    gLink.onLog(logline);
    gLink.begin({ PIN_LINK_SC, PIN_LINK_SI, PIN_LINK_SO }, LittleFS, ROM_PATH, LB_MOD_CNC_PENDANT, MODULE_FW_VERSION, MODULE_NAME);
    grbl.begin(fake, GRBL_RX_BUFFER);
    cnc.begin(gLink, grbl);
    isr_latency_max = 400;   // GBA sometimes slow to re-arm -> exercises retransmit

    printf("\n[1] power-up: GBA in multiboot BIOS\n");
    step_for(3.0);
    CHECK(bios_boot_ok, "multiboot upload accepted by BIOS model (CRC match)");
    CHECK(memcmp(bios_img.data(), LittleFS.mem.files[ROM_PATH].data(), LittleFS.mem.files[ROM_PATH].size()) == 0, "ROM image arrived byte-exact");
    CHECK(gLink.online(), "module link ONLINE after boot");
    CHECK(app.module_ok && strcmp(app.module_name, MODULE_NAME) == 0, "GBA got HELLO: '%s'", app.module_name);
    CHECK(app.have_status && app.state == CNC_ST_IDLE, "GBA shows status IDLE");
    CHECK(app.macro_count == MACRO_COUNT, "macro list loaded (%d)", app.macro_count);
    screenshot("01_jog_idle");

    printf("\n[2] continuous jog: hold RIGHT 1.0 s at 1000 mm/min\n");
    press(KEY_A); press(KEY_A);                     // 1 mm -> 10 mm -> CONT
    CHECK(app.step_idx == 0, "step mode = CONT");
    int32_t x0 = fake.pos[0];
    keys |= KEY_RIGHT; step_for(0.5);
    CHECK(fake.st != FakeGrbl::ALARM && !fake.planner.empty() && fake.planner.front().jog, "machine jogging while held");
    step_for(0.05); screenshot("02_jogging");
    step_for(0.45);
    keys &= (u16)~KEY_RIGHT; step_for(0.4);
    int32_t moved = fake.pos[0] - x0;
    CHECK(fake.planner.empty(), "machine stopped after release");
    CHECK(moved > 12000 && moved < 20000, "travel %.2f mm (expect ~16.7 at 1000 mm/min for 1 s)", moved / 1000.0);
    CHECK(fake.max_planned_s < 0.35, "planner never held more than %.2f s of jog travel", fake.max_planned_s);
    CHECK(app.wpos[0] == fake.pos[0], "GBA DRO X matches machine (%d um)", (int)app.wpos[0]);

    printf("\n[3] step jog: 1 mm, UP x3\n");
    press(KEY_A); press(KEY_A); press(KEY_A);       // CONT -> 0.01 -> 0.1 -> 1
    CHECK(app.step_idx == 3, "step = 1 mm");
    int32_t y0 = fake.pos[1];
    press(KEY_UP); press(KEY_UP); press(KEY_UP); step_for(0.5);
    CHECK(fake.pos[1] - y0 == 3000, "Y moved exactly 3.000 mm (got %.3f)", (fake.pos[1] - y0) / 1000.0);

    printf("\n[4] macros: Zero X+Y+Z\n");
    goto_screen(SCR_MACROS);
    CHECK(app.screen == SCR_MACROS, "on MACROS screen");
    for (int i = 0; i < 4; i++) press(KEY_DOWN);
    screenshot("03_macros");
    press(KEY_A); step_for(0.6);
    CHECK(app.wpos[0] == 0 && app.wpos[1] == 0 && app.wpos[2] == 0, "work position zeroed (%d,%d,%d)", (int)app.wpos[0], (int)app.wpos[1], (int)app.wpos[2]);
    CHECK(app.mpos[0] == fake.pos[0], "machine position unchanged");

    printf("\n[5] files + run job with hold/resume\n");
    goto_screen(SCR_FILES);
    step_for(0.3);
    CHECK(app.screen == SCR_FILES && app.file_count == 4, "file list loaded (%d files)", app.file_count);
    CHECK(strcmp(app.files[0].name, "bad_command.nc") == 0, "sorted: first = %s", app.files[0].name);
    press(KEY_DOWN);                                 // circle_test.nc
    press(KEY_A);
    CHECK(app.dialog == 1, "confirm dialog shown for %s", app.files[app.cursor[SCR_FILES]].name);
    screenshot("04_confirm_run");
    press(KEY_A); step_for(1.5);
    CHECK(cnc.jobActive() && fake.st != FakeGrbl::ALARM, "job running");
    CHECK(app.flags & CNC_FL_JOB_ACTIVE, "GBA sees job active, progress %u/1000", app.progress);
    goto_screen(SCR_JOG);
    screenshot("05_job_running");
    press(KEY_SELECT, 0.1); step_for(0.3);
    CHECK(fake.hold && app.state == CNC_ST_HOLD, "SELECT -> feed hold (GBA shows %s)", state_name(app.state));
    screenshot("06_hold");
    press(KEY_SELECT, 0.1); step_for(0.3);
    CHECK(!fake.hold && fake.resumes == 1, "SELECT tap -> resume");

    // overrides from the JOG screen while the job runs
    press(KEY_UP); press(KEY_UP); press(KEY_LEFT); step_for(0.4);
    CHECK(fake.ov_feed == 120 && fake.ov_spindle == 90, "D-pad overrides: feed %d%%, spindle %d%%", fake.ov_feed, fake.ov_spindle);
    CHECK(app.ov_feed == 120 && app.ov_spindle == 90, "GBA shows overrides F%u S%u", app.ov_feed, app.ov_spindle);
    CHECK(app.feed == 960, "reported feed follows override (%u mm/min)", app.feed);
    screenshot("08_overrides");
    press(KEY_A); step_for(0.4);
    CHECK(fake.ov_feed == 100 && fake.ov_spindle == 100 && app.ov_feed == 100, "A resets both to 100%%");
    CHECK(app.step_idx == 3, "A during a job did not change the jog step");
    step_for(25);
    CHECK(!cnc.jobActive(), "job finished");
    CHECK(fake.motion_lines == job_moves + 2, "all motion lines executed (%d/%d)", fake.motion_lines, job_moves + 2);
    CHECK(fake.max_rx <= 127, "GRBL RX buffer never overflowed (max %zu/128)", fake.max_rx);
    CHECK(strstr(app.msg, "Done") || strstr(app.msg, "done"), "GBA message: '%s'", app.msg);

    printf("\n[6] job + long-press SELECT = safe stop\n");
    goto_screen(SCR_FILES);
    press(KEY_A); press(KEY_A); step_for(1.0);
    CHECK(cnc.jobActive(), "job running again");
    int resets0 = fake.resets, alarms0 = fake.alarms;
    keys |= KEY_SELECT; step_for(1.0); keys &= (u16)~KEY_SELECT; step_for(1.0);
    CHECK(!cnc.jobActive(), "job stopped");
    CHECK(fake.resets == resets0 + 1 && fake.alarms == alarms0, "hold -> soft reset without alarm (position kept)");
    CHECK(fake.st == FakeGrbl::IDLE && app.state == CNC_ST_IDLE, "machine idle afterwards");

    printf("\n[7] link cable pulled mid-jog\n");
    goto_screen(SCR_JOG);
    while (app.step_idx != 0) press(KEY_A);
    keys |= KEY_UP; step_for(0.4);
    CHECK(!fake.planner.empty(), "jogging");
    cable = false;
    step_for(0.4);
    CHECK(fake.planner.empty(), "module dead-man cancelled the jog after cable loss");
    keys = 0; step_for(1.0);
    CHECK(!gLink.online(), "module noticed link loss");
    CHECK(!app.linked, "GBA shows NO LINK");
    screenshot("07_no_link");
    cable = true; step_for(1.5);
    CHECK(gLink.online() && app.module_ok, "reconnect: ROM still running, link back without re-boot");

    printf("\n[8] bad jobs\n");
    int al0 = fake.alarms;
    cnc.startJob("/gcode/too_long_line.nc"); step_for(1.0);
    CHECK(!cnc.jobActive(), "over-long line: job stopped");
    cnc.startJob("/gcode/bad_command.nc"); step_for(1.5);
    CHECK(!cnc.jobActive(), "job aborted on GRBL error");
    CHECK(fake.alarms == al0 && fake.st == FakeGrbl::IDLE, "error stop did not alarm the machine (position kept)");

    printf("\n[9] stress: 1 in 25 data words corrupted for 5 s while a job streams\n");
    uint16_t crc0 = app.crc_errors;
    corrupt_every = 25;
    cnc.startJob("/gcode/circle_test.nc");
    step_for(5);
    corrupt_every = 0;
    step_for(1);
    CHECK(app.crc_errors > crc0, "GBA detected %u corrupt frames", (unsigned)(app.crc_errors - crc0));
    CHECK(app.have_status && app.state == CNC_ST_RUN, "status still flowing (%s)", state_name(app.state));
    cnc.stop(); step_for(1);

    goto_screen(SCR_INFO); step_for(1.2);
    CHECK(app.words_per_s > 300 && app.module_fw == MODULE_FW_VERSION, "INFO: %u words/s, fw %u.%u", app.words_per_s, app.module_fw >> 8, app.module_fw & 0xFF);
    screenshot("09_info");

    printf("\nlink stats: words=%u unarmed(retransmit)=%u retries=%u module_crc_err=%u\n",
           words_done, words_unarmed, (unsigned)gLink.retries(), (unsigned)gLink.crcErrors());
    printf("grbl: holds=%d resumes=%d jog_cancels=%d resets=%d alarms=%d max_rx=%zu\n",
           fake.holds, fake.resumes, fake.jog_cancels, fake.resets, fake.alarms, fake.max_rx);
    printf("module log:\n"); for (auto &l : esp_log) printf("  %s\n", l.c_str());
    printf(failures ? "\n%d FAILURES\n" : "\nALL CHECKS PASSED\n", failures);
    return failures != 0;
}
