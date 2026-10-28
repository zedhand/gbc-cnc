// Bridges the GBA (LinkBus packets) and GRBL: jogging, job streaming, macros, lists.
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "LinkMaster.h"
#include "Grbl.h"
#include "config.h"

class CncModule {
public:
    void begin(LinkMaster &link, Grbl &grbl);
    void loop();

    // also used by the web UI
    bool startJob(const String &path);
    void stop();                       // feed hold -> reset; works during jobs, jogs, macros
    bool jobActive() const { return _job.active; }
    const String &jobName() const { return _job.name; }
    uint16_t progress() const;
    String lastMessage() const { return _lastMsg; }

private:
    static void onPacketThunk(uint8_t t, const uint8_t *p, uint8_t n, void *c) { ((CncModule *)c)->onPacket(t, p, n); }
    static void onLinkThunk(bool on, void *c) { ((CncModule *)c)->onLink(on); }
    static void onRespThunk(Grbl::Tag t, int e, void *c) { ((CncModule *)c)->onResponse(t, e); }
    static void onAlarmThunk(int code, void *c) { ((CncModule *)c)->onAlarm(code); }
    static void onTextThunk(const char *l, void *c) { ((CncModule *)c)->onText(l); }

    void onPacket(uint8_t type, const uint8_t *pl, uint8_t len);
    void onLink(bool online);
    void onResponse(Grbl::Tag tag, int err);
    void onAlarm(int code);
    void onText(const char *line);

    void sendStatus(bool force);
    void sendEvent(uint8_t ev, uint16_t arg);
    void sendList(uint8_t kind);
    void runListItem(uint8_t kind, uint8_t index);
    void message(uint8_t level, const char *fmt, ...);

    bool jogAllowed();
    void jogStep(uint8_t axis, int32_t um, uint16_t feed);
    void jogContTick();
    void jogCancel();

    void streamJob();
    bool readJobLine(char *out, size_t cap);
    void finishJob(uint8_t ev, uint16_t arg);

    void queueUserLines(const char *lines);
    void drainUserLines();

    LinkMaster *_link = nullptr;
    Grbl       *_grbl = nullptr;

    struct Job {
        bool     active = false, eof = false;
        File     f;
        String   name;
        uint32_t size = 0, lines = 0;
        char     next[96];
        bool     haveNext = false;
    } _job;

    bool     _stopping = false;
    uint32_t _stopStart = 0;

    // continuous jog
    bool     _jogCont = false;
    int8_t   _jogDir[3] = {0, 0, 0};
    uint16_t _jogFeed = 0;
    uint32_t _jogBeat = 0, _jogNextSeg = 0, _jogStart = 0;
    uint16_t _jogSent = 0;

    // queued user/macro lines waiting for GRBL buffer room
    static const uint8_t ULQ = 12;
    String   _ul[ULQ];
    uint8_t  _ulHead = 0, _ulCount = 0;

    uint32_t _lastStatusTx = 0, _lastStatusSrc = 0;
    uint8_t  _lastState = 255;
    String   _lastMsg;
    String   _files[MAX_FILES];
    uint8_t  _fileCount = 0;
};
