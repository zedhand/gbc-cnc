#include "LinkMaster.h"
#include <stdarg.h>

// ---------------------------------------------------------------------------------------
// Fast GPIO. ESP8266: direct set/clear registers. Everything else: Arduino calls (an ESP32
// port works as-is, just with a slower clock).
// ---------------------------------------------------------------------------------------
#if defined(ESP8266)
#define PIN_HI(p)  (GPOS = (1u << (p)))
#define PIN_LO(p)  (GPOC = (1u << (p)))
#define PIN_RD(p)  ((GPI >> (p)) & 1u)
#else
#define PIN_HI(p)  digitalWrite((p), HIGH)
#define PIN_LO(p)  digitalWrite((p), LOW)
#define PIN_RD(p)  (digitalRead(p) ? 1u : 0u)
#endif

// ---- multiboot C glue ------------------------------------------------------------------
static uint32_t mb_xfer(void *c, uint32_t w) { return static_cast<LinkMaster *>(c)->xfer(w); }
static int mb_read(void *c, uint32_t off, uint8_t *b, uint32_t n) { return static_cast<LinkMaster *>(c)->readRom(off, b, n); }
static void mb_delay(void *, uint32_t ms) { delay(ms); }
static void mb_yield(void *) { yield(); }

void LinkMaster::begin(const Pins &pins, fs::FS &fs, const char *romPath,
                       uint16_t moduleId, uint16_t fwVersion, const char *moduleName)
{
    _p = pins;
    _fs = &fs;
    _romPath = romPath;
    _modId = moduleId;
    _fwVer = fwVersion;
    strncpy(_name, moduleName, sizeof(_name) - 1);

    pinMode(_p.sc, OUTPUT);
    pinMode(_p.si, OUTPUT);
    pinMode(_p.so, INPUT_PULLUP);   // floating/absent GBA reads as 0xFFFFFFFF = invalid word
    PIN_HI(_p.sc);                  // SC idles high
    PIN_HI(_p.si);

    lb_ring_init(&_tx, _txBuf, sizeof(_txBuf));
    lb_parser_init(&_parser);
    setState(PROBING);
}

// One 32-bit Normal-mode transfer, MSB first. SC idles high; the GBA shifts SO out on the
// falling edge and samples SI on the rising edge.
uint32_t IRAM_ATTR LinkMaster::xfer(uint32_t out)
{
    uint32_t in = 0;
    const uint8_t h = bitHalfUs;
    noInterrupts();
    for (int i = 31; i >= 0; i--) {
        PIN_LO(_p.sc);
        if ((out >> i) & 1u) PIN_HI(_p.si); else PIN_LO(_p.si);
        delayMicroseconds(h);
        PIN_HI(_p.sc);
        delayMicroseconds(h);
        in = (in << 1) | PIN_RD(_p.so);
    }
    interrupts();
    _lastXfer = micros();
    return in;
}

int LinkMaster::readRom(uint32_t off, uint8_t *buf, uint32_t n)
{
    if (!_rom) return 0;
    uint32_t size = _rom.size();
    memset(buf, 0, n);
    if (off >= size) return 1;
    if (_rom.position() != off && !_rom.seek(off)) return 0;
    uint32_t want = (off + n > size) ? size - off : n;
    return _rom.read(buf, want) == want;
}

void LinkMaster::setState(State s)
{
    State old = _state;
    _state = s;
    _invalid = 0;
    _pending = false;
    if (s == ONLINE) {
        lb_parser_init(&_parser);
        sendHello();
        _lastHello = millis();
    }
    if (old == ONLINE && s != ONLINE) {
        lb_ring_init(&_tx, _txBuf, sizeof(_txBuf));   // stale frames are meaningless to a new ROM
    }
    if ((old == ONLINE) != (s == ONLINE) && _linkFn) _linkFn(s == ONLINE, _linkCtx);
}

void LinkMaster::doBoot()
{
    _rom = _fs->open(_romPath, "r");
    if (!_rom) {
        snprintf(_bootMsg, sizeof(_bootMsg), "ROM missing: %s", _romPath);
        log("multiboot: %s", _bootMsg);
        _bootBackoff = millis() + 3000;
        setState(PROBING);
        return;
    }
    uint32_t t0 = millis();
    mb_io_t io = { mb_xfer, mb_read, mb_delay, mb_yield, this };
    mb_result_t r = mb_upload(&io, _rom.size(), 20);
    uint32_t sz = _rom.size();
    _rom.close();
    if (r == MB_OK) {
        snprintf(_bootMsg, sizeof(_bootMsg), "ok, %lu B in %lu ms", (unsigned long)sz, (unsigned long)(millis() - t0));
        _bootBackoff = millis() + 400;   // give the ROM time to start and arm SIO
    } else {
        snprintf(_bootMsg, sizeof(_bootMsg), "failed: %s", mb_strerror(r));
        _bootBackoff = millis() + 1500;
    }
    log("multiboot: %s", _bootMsg);
    setState(PROBING);
}

void LinkMaster::loop()
{
    uint32_t nowMs = millis();

    if (_state == PROBING) {
        if ((int32_t)(nowMs - _bootBackoff) < 0 || nowMs - _lastProbe < 100) return;
        _lastProbe = nowMs;
        uint32_t r = xfer(0x00006202);
        uint8_t tmp[3];
        if ((r >> 16) == 0x7202) {
            log("GBA in multiboot BIOS, uploading %s", _romPath);
            setState(BOOTING);
            doBoot();
        } else if (lb_word_unpack(r, tmp) >= 0) {
            setState(ONLINE);
        }
        return;
    }

    if (_state != ONLINE) return;

    bool busy = _pending || _rxActive || lb_ring_used(&_tx) > 0;
    uint32_t gap = busy ? gapBusyUs : gapIdleUs;
    if (micros() - _lastXfer < gap) return;

    uint32_t w;
    if (_pending) {
        w = _pendingWord;
    } else {
        uint8_t b[3];
        int n = 0;
        while (n < 3 && lb_ring_get(&_tx, &b[n])) n++;
        w = lb_word_pack(b, n);
    }

    uint32_t r = xfer(w);
    uint8_t in[3];
    int n = lb_word_unpack(r, in);

    if (n < 0) {
        // GBA wasn't armed: our word was not taken. Keep it for retransmit.
        if ((w & 0x03000000u) != 0) { _pendingWord = w; _pending = true; _retries++; }
        if ((r >> 16) == 0x7202) { log("GBA rebooted into BIOS"); setState(PROBING); return; }
        if (++_invalid >= offlineAfter) { log("GBA link lost"); setState(PROBING); }
        return;
    }

    _invalid = 0;
    _pending = false;
    _rxActive = n > 0;
    if (n == 0) {
        lb_parser_idle(&_parser);
    } else {
        for (int i = 0; i < n; i++)
            if (lb_parser_feed(&_parser, in[i]))
                handlePacket(_parser.buf[0], _parser.buf + 1, _parser.len - 1);
    }

    if (nowMs - _lastHello > 3000) { sendHello(); _lastHello = nowMs; }
}

void LinkMaster::handlePacket(uint8_t type, const uint8_t *pl, uint8_t len)
{
    switch (type) {
    case LB_HELLO_REQ: sendHello(); return;
    case LB_PING:      send(LB_PONG, pl, len); return;
    default:
        if (_pktFn) _pktFn(type, pl, len, _pktCtx);
    }
}

bool LinkMaster::send(uint8_t type, const void *payload, size_t len)
{
    if (_state != ONLINE) return false;
    uint8_t frame[LB_MAX_FRAME];
    size_t n = lb_encode(frame, sizeof(frame), type, payload, len);
    return n && lb_ring_write(&_tx, frame, (uint16_t)n);
}

bool LinkMaster::sendLog(uint8_t level, const char *text)
{
    uint8_t b[LB_MAX_PAYLOAD];
    b[0] = level;
    size_t n = strnlen(text, LB_MAX_PAYLOAD - 2);
    memcpy(b + 1, text, n);
    b[1 + n] = 0;
    return send(LB_LOG, b, n + 2);
}

bool LinkMaster::sendHello()
{
    uint8_t b[5 + sizeof(_name)];
    b[0] = LB_PROTO_VERSION;
    lb_put16(b + 1, _modId);
    lb_put16(b + 3, _fwVer);
    size_t n = strnlen(_name, sizeof(_name) - 1);
    memcpy(b + 5, _name, n);
    b[5 + n] = 0;
    return send(LB_HELLO, b, 6 + n);
}

void LinkMaster::log(const char *fmt, ...)
{
    if (!_logFn) return;
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    _logFn(buf);
}
