/*
 * LinkMaster — module side of LinkBus v1.
 *
 * The module is the SIO clock master, the GBA is the Normal-mode 32-bit slave. One set of
 * three wires does both jobs:
 *   PROBING : sends the multiboot recognition word; a GBA sitting in the BIOS boot screen
 *             answers 0x7202 -> we upload the module's ROM. A GBA already running a LinkBus
 *             ROM answers with LinkBus words -> ONLINE straight away.
 *   ONLINE  : exchanges LinkBus words. A word that comes back invalid means the GBA was not
 *             armed, so ours was not received: it is retransmitted. Too many invalid replies,
 *             or a BIOS reply, drops back to PROBING (GBA rebooted / unplugged).
 */
#pragma once
#include <Arduino.h>
#include <FS.h>

extern "C" {
#include "linkbus_proto.h"
#include "linkbus_ids.h"
#include "lb_multiboot.h"
}

class LinkMaster {
public:
    struct Pins {
        uint8_t sc;   // clock out      -> GBA SC (link pin 5)
        uint8_t si;   // data to GBA    -> GBA SI (link pin 3)
        uint8_t so;   // data from GBA  <- GBA SO (link pin 2)
    };
    enum State : uint8_t { PROBING, BOOTING, ONLINE };

    typedef void (*PacketFn)(uint8_t type, const uint8_t *payload, uint8_t len, void *ctx);
    typedef void (*LinkFn)(bool online, void *ctx);
    typedef void (*LogFn)(const char *msg);

    void begin(const Pins &pins, fs::FS &fs, const char *romPath,
               uint16_t moduleId, uint16_t fwVersion, const char *moduleName);
    void loop();

    bool send(uint8_t type, const void *payload, size_t len);  // false if TX buffer full
    bool sendLog(uint8_t level, const char *text);
    bool sendHello();

    void onPacket(PacketFn fn, void *ctx) { _pktFn = fn; _pktCtx = ctx; }
    void onLink(LinkFn fn, void *ctx)     { _linkFn = fn; _linkCtx = ctx; }
    void onLog(LogFn fn)                  { _logFn = fn; }

    State    state() const    { return _state; }
    bool     online() const   { return _state == ONLINE; }
    uint16_t txFree() const   { return lb_ring_free(&_tx); }
    uint32_t crcErrors() const{ return _parser.crc_errors; }
    uint32_t retries() const  { return _retries; }
    const char *lastBootResult() const { return _bootMsg; }

    // tuning
    uint8_t  bitHalfUs   = 2;     // ~200 kHz SC. GBA slave is fine well beyond this.
    uint16_t gapBusyUs   = 300;   // between words while data is moving (GBA ISR re-arm time)
    uint16_t gapIdleUs   = 2000;  // idle poll interval
    uint16_t offlineAfter = 400;  // consecutive invalid replies before giving up (~0.15 s busy)

    // used by the multiboot C callbacks
    uint32_t xfer(uint32_t out);
    int      readRom(uint32_t off, uint8_t *buf, uint32_t n);

private:
    void setState(State s);
    void doBoot();
    void handlePacket(uint8_t type, const uint8_t *pl, uint8_t len);
    void log(const char *fmt, ...);

    Pins      _p{};
    fs::FS   *_fs = nullptr;
    const char *_romPath = nullptr;
    File      _rom;
    uint16_t  _modId = 0, _fwVer = 0;
    char      _name[24] = {0};

    State     _state = PROBING;
    uint32_t  _lastXfer = 0, _lastProbe = 0, _lastHello = 0, _bootBackoff = 0;
    uint32_t  _pendingWord = 0;
    bool      _pending = false;
    uint16_t  _invalid = 0;
    uint32_t  _retries = 0;
    bool      _rxActive = false;

    uint8_t     _txBuf[2048];
    mutable lb_ring_t _tx;
    lb_parser_t _parser;

    PacketFn _pktFn = nullptr;  void *_pktCtx = nullptr;
    LinkFn   _linkFn = nullptr; void *_linkCtx = nullptr;
    LogFn    _logFn = nullptr;
    char     _bootMsg[48] = "not attempted";
};
