#include "Grbl.h"

// Parse a GRBL decimal ("-12.345", "0.5", "3") into micrometres, no floats.
static int32_t parseUm(const char *&p)
{
    bool neg = false;
    if (*p == '-') { neg = true; p++; } else if (*p == '+') p++;
    int32_t ip = 0, fp = 0, scale = 1000;
    while (*p >= '0' && *p <= '9') ip = ip * 10 + (*p++ - '0');
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (scale > 1) { scale /= 10; fp += (*p - '0') * scale; }
            p++;
        }
    }
    int32_t v = ip * 1000 + fp;
    return neg ? -v : v;
}

static void parse3(const char *p, int32_t out[3])
{
    for (int i = 0; i < 3; i++) {
        out[i] = parseUm(p);
        if (*p == ',') p++;
    }
}

const char *Grbl::stateName(uint8_t s)
{
    static const char *n[] = { "Offline", "Idle", "Run", "Hold", "Jog", "Alarm", "Door", "Check", "Home", "Sleep" };
    return s < 10 ? n[s] : "?";
}

void Grbl::begin(Stream &s, uint16_t rxBuffer)
{
    _s = &s;
    _rxBuf = rxBuffer;
    // The ESP8266 boot ROM prints junk at 74880 baud on TX at power-up. A bare newline makes
    // GRBL discard it as one bad line; its "error" reply is ignored (nothing in flight yet).
    _s->write('\n');
    _lastResp = millis();
}

uint8_t Grbl::inFlight(Tag t) const
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < _qCount; i++)
        if (_qTag[(_qHead + i) % QMAX] == t) n++;
    return n;
}

bool Grbl::canQueue(size_t len) const
{
    return _qCount < QMAX && _qBytes + len + 1 < _rxBuf;
}

bool Grbl::queueLine(const char *line, Tag tag)
{
    size_t len = strlen(line);
    if (len == 0 || len > 80 || !canQueue(len)) return false;
    uint8_t slot = (_qHead + _qCount) % QMAX;
    _qLen[slot] = (uint8_t)(len + 1);
    _qTag[slot] = tag;
    _qCount++;
    _qBytes += len + 1;
    if (_qCount == 1) _lastResp = millis();   // start the stuck-counter clock now
    _s->write((const uint8_t *)line, len);
    _s->write('\n');
    return true;
}

void Grbl::realtime(uint8_t b) { _s->write(b); }

void Grbl::resetCounter()
{
    _qHead = _qCount = 0;
    _qBytes = 0;
}

void Grbl::pop(int err)
{
    if (_qCount == 0) return;   // unsolicited ok/error (boot junk, flushed jog line)
    Tag t = (Tag)_qTag[_qHead];
    _qBytes -= _qLen[_qHead];
    _qHead = (_qHead + 1) % QMAX;
    _qCount--;
    if (_respFn) _respFn(t, err, _respCtx);
}

void Grbl::loop()
{
    while (_s->available()) {
        char c = (char)_s->read();
        if (c == '\r') continue;
        if (c == '\n') {
            _line[_lineLen] = 0;
            if (_lineLen) handleLine(_line);
            _lineLen = 0;
        } else if (_lineLen < sizeof(_line) - 1) {
            _line[_lineLen++] = c;
        }
    }

    uint32_t now = millis();
    if (now - _lastPoll >= _pollMs) {
        _lastPoll = now;
        _s->write('?');
    }
    if (_st.state != OFFLINE && now - _lastStatus > 2000) {
        _st.state = OFFLINE;
        resetCounter();
    }
    // A jog cancel (0x85) can drop queued jog lines without an "ok" for each. If GRBL is
    // idle and nothing has answered for a second, the counter is stale: forget it.
    if (_qCount && _st.state == IDLE && now - _lastResp > 1000) resetCounter();
}

void Grbl::handleLine(char *line)
{
    if (line[0] == '<') { parseStatus(line + 1); return; }

    _lastResp = millis();
    if (strcmp(line, "ok") == 0)                { pop(0); return; }
    if (strncmp(line, "error:", 6) == 0)        { pop(atoi(line + 6)); return; }
    if (strncmp(line, "ALARM:", 6) == 0) {
        if (_alarmFn) _alarmFn(atoi(line + 6), _alarmCtx);
        return;
    }
    if (strncmp(line, "Grbl ", 5) == 0) {       // reset banner: everything in flight is gone
        resetCounter();
        _haveWco = false;
    }
    if (_textFn) _textFn(line, _textCtx);
}

void Grbl::parseStatus(char *s)
{
    char *end = strchr(s, '>');
    if (end) *end = 0;

    bool gotM = false, gotW = false;
    int32_t pos[3];
    char *save = nullptr;
    for (char *tok = strtok_r(s, "|", &save); tok; tok = strtok_r(nullptr, "|", &save)) {
        if (tok == s) {   // first field: state[:sub]
            static const char *names[] = { "Idle", "Run", "Hold", "Jog", "Alarm", "Door", "Check", "Home", "Sleep" };
            uint8_t st = OFFLINE, sub = 0;
            char *colon = strchr(tok, ':');
            if (colon) { *colon = 0; sub = (uint8_t)atoi(colon + 1); }
            for (uint8_t i = 0; i < 9; i++) if (strcmp(tok, names[i]) == 0) st = i + 1;
            _st.state = st;
            _st.sub = sub;
        } else if (strncmp(tok, "MPos:", 5) == 0) { parse3(tok + 5, pos); gotM = true; }
        else if (strncmp(tok, "WPos:", 5) == 0)   { parse3(tok + 5, pos); gotW = true; }
        else if (strncmp(tok, "WCO:", 4) == 0)    { parse3(tok + 4, _st.wco); _haveWco = true; }
        else if (strncmp(tok, "FS:", 3) == 0) {
            const char *p = tok + 3;
            _st.feed = (uint16_t)atoi(p);
            p = strchr(p, ',');
            _st.spindle = p ? (uint16_t)atoi(p + 1) : 0;
        } else if (strncmp(tok, "F:", 2) == 0) { _st.feed = (uint16_t)atoi(tok + 2); }
        else if (strncmp(tok, "Ov:", 3) == 0) {
            const char *p = tok + 3;
            _st.ovFeed = (uint8_t)atoi(p);
            if ((p = strchr(p, ','))) { _st.ovRapid = (uint8_t)atoi(++p);
                if ((p = strchr(p, ','))) _st.ovSpindle = (uint8_t)atoi(++p); }
        }
    }

    for (int i = 0; i < 3; i++) {
        if (gotM) { _st.mpos[i] = pos[i]; _st.wpos[i] = pos[i] - (_haveWco ? _st.wco[i] : 0); }
        if (gotW) { _st.wpos[i] = pos[i]; _st.mpos[i] = pos[i] + (_haveWco ? _st.wco[i] : 0); }
    }
    _st.updatedMs = _lastStatus = millis();
}
