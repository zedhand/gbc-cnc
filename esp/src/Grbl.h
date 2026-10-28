// GRBL 1.1 serial link: character-counting sender, realtime commands, status parsing.
#pragma once
#include <Arduino.h>

class Grbl {
public:
    enum Tag : uint8_t { TAG_INTERNAL, TAG_USER, TAG_JOB, TAG_JOG };
    enum St  : uint8_t { OFFLINE = 0, IDLE, RUN, HOLD, JOG, ALARM, DOOR, CHECK, HOME, SLEEP };

    struct Status {
        uint8_t  state = OFFLINE, sub = 0;
        int32_t  mpos[3] = {0, 0, 0}, wpos[3] = {0, 0, 0}, wco[3] = {0, 0, 0};
        uint16_t feed = 0, spindle = 0;
        uint8_t  ovFeed = 100, ovRapid = 100, ovSpindle = 100;   // GRBL reports Ov: every few reports
        uint32_t updatedMs = 0;
    };

    // ok = err==0; err = GRBL error code otherwise
    typedef void (*ResponseFn)(Tag tag, int err, void *ctx);
    typedef void (*AlarmFn)(int code, void *ctx);
    typedef void (*TextFn)(const char *line, void *ctx);   // [MSG:..], banners, unknown lines

    void begin(Stream &s, uint16_t rxBuffer);
    void loop();

    bool canQueue(size_t len) const;                 // would this line fit right now?
    bool queueLine(const char *line, Tag tag);       // appends '\n'; false if no room
    void realtime(uint8_t b);                        // '!', '~', '?', 0x18, 0x85 ...
    void resetCounter();                             // forget in-flight lines (after reset)

    uint8_t inFlight() const { return _qCount; }
    uint8_t inFlight(Tag t) const;
    const Status &status() const { return _st; }
    bool    online() const { return _st.state != OFFLINE; }

    void setPollMs(uint16_t ms) { _pollMs = ms; }
    void onResponse(ResponseFn f, void *c) { _respFn = f; _respCtx = c; }
    void onAlarm(AlarmFn f, void *c)       { _alarmFn = f; _alarmCtx = c; }
    void onText(TextFn f, void *c)         { _textFn = f; _textCtx = c; }

    static const char *stateName(uint8_t s);

private:
    void handleLine(char *line);
    void parseStatus(char *s);
    void pop(int err);

    Stream  *_s = nullptr;
    uint16_t _rxBuf = 128;
    uint16_t _pollMs = 200;
    uint32_t _lastPoll = 0, _lastResp = 0, _lastStatus = 0;

    static const uint8_t QMAX = 32;
    uint8_t  _qLen[QMAX], _qTag[QMAX];
    uint8_t  _qHead = 0, _qCount = 0;
    uint16_t _qBytes = 0;

    char     _line[128];
    uint8_t  _lineLen = 0;
    bool     _haveWco = false;

    Status   _st;
    ResponseFn _respFn = nullptr; void *_respCtx = nullptr;
    AlarmFn  _alarmFn = nullptr;  void *_alarmCtx = nullptr;
    TextFn   _textFn = nullptr;   void *_textCtx = nullptr;
};
