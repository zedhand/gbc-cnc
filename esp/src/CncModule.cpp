#include "CncModule.h"
#include <stdarg.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

// ---- helpers ----------------------------------------------------------------------------

// micrometres -> "-12.345"
static void fmtUm(char *out, int32_t um)
{
    bool neg = um < 0;
    uint32_t a = neg ? (uint32_t)(-um) : (uint32_t)um;
    sprintf(out, "%s%lu.%03lu", neg ? "-" : "", (unsigned long)(a / 1000), (unsigned long)(a % 1000));
}

static const char *grblError(int e)
{
    switch (e) {
    case 1:  return "expected command letter";
    case 2:  return "bad number format";
    case 3:  return "invalid $ statement";
    case 8:  return "not idle";
    case 9:  return "locked: alarm or jog";
    case 15: return "jog exceeds travel";
    case 16: return "invalid jog command";
    case 20: return "unsupported command";
    case 22: return "feed rate not set";
    case 24: return "two axis words";
    case 33: return "invalid motion target";
    default: return "";
    }
}

static const char *grblAlarm(int a)
{
    switch (a) {
    case 1: return "hard limit";
    case 2: return "soft limit";
    case 3: return "reset while moving";
    case 4: return "probe not clear";
    case 5: return "probe missed";
    case 6: case 7: case 8: case 9: return "homing failed";
    default: return "";
    }
}

static const char AXIS[3] = { 'X', 'Y', 'Z' };
static const int8_t INV[3] = { JOG_INVERT_X ? -1 : 1, JOG_INVERT_Y ? -1 : 1, JOG_INVERT_Z ? -1 : 1 };

// ---- setup ---------------------------------------------------------------------------

static uint8_t s_listKind = 0xFF, s_listIdx = 0;

void CncModule::begin(LinkMaster &link, Grbl &grbl)
{
    _link = &link;
    _grbl = &grbl;
    _link->onPacket(onPacketThunk, this);
    _link->onLink(onLinkThunk, this);
    _grbl->onResponse(onRespThunk, this);
    _grbl->onAlarm(onAlarmThunk, this);
    _grbl->onText(onTextThunk, this);
    LittleFS.mkdir(GCODE_DIR);
}

void CncModule::loop()
{
    const Grbl::Status &st = _grbl->status();
    bool fast = _job.active || _jogCont || st.state == Grbl::RUN || st.state == Grbl::JOG;
    _grbl->setPollMs(fast ? GRBL_POLL_FAST_MS : GRBL_POLL_IDLE_MS);

    if (_stopping) {
        // Only trust a status report taken after the feed hold went out: an older one can
        // still say Idle while the machine is moving, and resetting then costs position.
        bool fresh = (int32_t)(st.updatedMs - _stopStart) > 20;
        bool settled = (st.state == Grbl::HOLD && st.sub == 0) || st.state == Grbl::IDLE ||
                       st.state == Grbl::ALARM || st.state == Grbl::OFFLINE || st.state == Grbl::CHECK;
        if ((fresh && settled) || millis() - _stopStart > 8000) {
            _grbl->realtime(0x18);          // soft reset from Hold:0 keeps position
            _grbl->resetCounter();
            _stopping = false;
            if (_job.active) finishJob(CNC_EV_ABORTED, 0);
            message(1, "Stopped");
        }
    }

    jogContTick();
    drainUserLines();
    streamJob();

    // paced list transmission (the TX buffer is 2 KB, a full file list is bigger)
    while (s_listKind != 0xFF && _link->online() && _link->txFree() > 80) {
        uint8_t total = s_listKind == CNC_LIST_FILES ? _fileCount : MACRO_COUNT;
        if (s_listIdx >= total) {
            uint8_t b[2] = { s_listKind, total };
            if (_link->send(LB_LIST_END, b, 2)) s_listKind = 0xFF;
            break;
        }
        uint8_t b[LB_MAX_PAYLOAD];
        b[0] = s_listKind; b[1] = s_listIdx; b[2] = total;
        const char *name;
        uint32_t arg;
        if (s_listKind == CNC_LIST_FILES) {
            name = _files[s_listIdx].c_str();
            File f = LittleFS.open(String(GCODE_DIR "/") + _files[s_listIdx], "r");
            arg = f ? f.size() : 0;
        } else {
            name = MACROS[s_listIdx].name;
            arg = MACROS[s_listIdx].confirm ? 1 : 0;
        }
        lb_put32(b + 3, arg);
        size_t n = strnlen(name, 40);
        memcpy(b + 7, name, n);
        b[7 + n] = 0;
        if (!_link->send(LB_LIST_ENTRY, b, 8 + n)) break;
        s_listIdx++;
    }

    sendStatus(false);
}

// ---- GBA -> module ------------------------------------------------------------------------

void CncModule::onLink(bool online)
{
    if (!online) jogCancel();   // cable pulled mid-jog: stop moving. Jobs keep running.
    else { _lastState = 255; sendStatus(true); }
}

void CncModule::onPacket(uint8_t type, const uint8_t *pl, uint8_t len)
{
    switch (type) {
    case CNC_RT: {
        if (len < 1) return;
        uint8_t b = pl[0];
        if (b == 0x18) { stop(); return; }                   // never hard-reset a moving job
        if (b == '!' || b == '~' || b == '?' || b == 0x85 || b == 0x84 || (b >= 0x90 && b <= 0xA1))
            _grbl->realtime(b);
        return;
    }
    case CNC_LINE:
        if (len < 2 || _job.active || _stopping) { sendEvent(CNC_EV_REFUSED, 0); return; }
        queueUserLines((const char *)pl);
        return;

    case CNC_JOG_STEP:
        if (len < 7) return;
        jogStep(pl[0], (int32_t)lb_get32(pl + 1), lb_get16(pl + 5));
        return;

    case CNC_JOG_CONT: {
        if (len < 5) return;
        int8_t d[3] = { (int8_t)pl[0], (int8_t)pl[1], (int8_t)pl[2] };
        uint16_t feed = lb_get16(pl + 3);
        if (!d[0] && !d[1] && !d[2]) { jogCancel(); return; }
        if (!jogAllowed() && !_jogCont) { sendEvent(CNC_EV_REFUSED, 0); return; }
        bool changed = _jogCont && (memcmp(d, _jogDir, 3) != 0 || feed != _jogFeed);
        if (changed) {
            jogCancel();
            _jogStart = _jogNextSeg = millis() + 150;   // let the cancel finish first
            _jogSent = 0;
        } else if (!_jogCont) {
            _jogStart = _jogNextSeg = millis();
            _jogSent = 0;
        }
        memcpy(_jogDir, d, 3);
        _jogFeed = feed;
        _jogBeat = millis();
        _jogCont = true;
        return;
    }
    case CNC_JOG_STOP:
        jogCancel();
        return;

    case CNC_JOB_STOP:
        stop();
        return;

    case LB_LIST_REQ:
        if (len >= 1) sendList(pl[0]);
        return;

    case LB_LIST_RUN:
        if (len >= 2) runListItem(pl[0], pl[1]);
        return;
    }
}

// ---- jogging ---------------------------------------------------------------------------

bool CncModule::jogAllowed()
{
    uint8_t s = _grbl->status().state;
    return !_job.active && !_stopping && (s == Grbl::IDLE || s == Grbl::JOG);
}

void CncModule::jogStep(uint8_t axis, int32_t um, uint16_t feed)
{
    if (axis > 2 || um == 0) return;
    if (!jogAllowed()) { sendEvent(CNC_EV_REFUSED, 0); return; }
    uint16_t maxF = axis == 2 ? JOG_MAX_FEED_Z : JOG_MAX_FEED_XY;
    if (feed == 0 || feed > maxF) feed = maxF;
    char d[16], line[48];
    fmtUm(d, um * INV[axis]);
    snprintf(line, sizeof(line), "$J=G91 G21 %c%s F%u", AXIS[axis], d, feed);
    if (!_grbl->queueLine(line, Grbl::TAG_JOG)) sendEvent(CNC_EV_REFUSED, 0);
}

void CncModule::jogContTick()
{
    if (!_jogCont) return;
    uint32_t now = millis();
    if (now - _jogBeat > JOG_DEADMAN_MS) { jogCancel(); return; }     // button released / link lost
    if ((int32_t)(now - _jogNextSeg) < 0) return;
    if (_grbl->inFlight(Grbl::TAG_JOG) >= JOG_CONT_LEAD) return;     // don't run ahead of GRBL

    uint8_t axes = (_jogDir[0] != 0) + (_jogDir[1] != 0) + (_jogDir[2] != 0);
    uint16_t feed = _jogFeed;
    uint16_t maxF = _jogDir[2] ? JOG_MAX_FEED_Z : JOG_MAX_FEED_XY;
    if (feed == 0 || feed > maxF) feed = maxF;

    // travel per segment at this feed, split so the vector length matches
    int32_t um = (int32_t)feed * JOG_CONT_SEG_MS / 60;
    if (axes == 2) um = um * 707 / 1000;
    if (axes == 3) um = um * 577 / 1000;
    if (um < 5) um = 5;

    char line[80] = "$J=G91 G21", d[16];
    for (int i = 0; i < 3; i++) {
        if (!_jogDir[i]) continue;
        fmtUm(d, um * _jogDir[i] * INV[i]);
        size_t n = strlen(line);
        snprintf(line + n, sizeof(line) - n, " %c%s", AXIS[i], d);
    }
    size_t n = strlen(line);
    snprintf(line + n, sizeof(line) - n, " F%u", feed);

    if (_grbl->queueLine(line, Grbl::TAG_JOG)) {
        // JOG_CONT_LEAD segments go out at once, then exactly one per segment time, so the
        // planner never holds more than ~LEAD segments of travel (cancel stops it short).
        _jogSent++;
        _jogNextSeg = _jogSent < JOG_CONT_LEAD ? now
                    : _jogStart + (uint32_t)(_jogSent - JOG_CONT_LEAD + 1) * JOG_CONT_SEG_MS;
    }
}

void CncModule::jogCancel()
{
    bool wasJogging = _jogCont || _grbl->status().state == Grbl::JOG || _grbl->inFlight(Grbl::TAG_JOG);
    _jogCont = false;
    memset(_jogDir, 0, sizeof(_jogDir));
    if (wasJogging) _grbl->realtime(0x85);
}

// ---- jobs ------------------------------------------------------------------------------

bool CncModule::startJob(const String &path)
{
    uint8_t s = _grbl->status().state;
    if (_job.active || _stopping || !(s == Grbl::IDLE || s == Grbl::CHECK)) {
        message(2, "Can't start: machine %s", Grbl::stateName(s));
        sendEvent(CNC_EV_REFUSED, 0);
        return false;
    }
    File f = LittleFS.open(path, "r");
    if (!f) { message(2, "Can't open %s", path.c_str()); return false; }
    jogCancel();
    _job.f = f;
    _job.name = path.substring(path.lastIndexOf('/') + 1);
    _job.size = f.size();
    _job.lines = 0;
    _job.eof = _job.haveNext = false;
    _job.active = true;
    sendEvent(CNC_EV_STARTED, 0);
    message(0, "Running %s", _job.name.c_str());
    return true;
}

// Strip comments and whitespace; skip blank lines and '%'. false at end of file.
bool CncModule::readJobLine(char *out, size_t cap)
{
    while (_job.f.available()) {
        size_t n = 0;
        bool paren = false, semi = false, overflow = false;
        while (_job.f.available()) {
            int c = _job.f.read();
            if (c == '\n') break;
            if (c == '\r' || semi) continue;
            if (paren) { if (c == ')') paren = false; continue; }
            if (c == '(') { paren = true; continue; }
            if (c == ';') { semi = true; continue; }
            if (c == ' ' || c == '\t' || c == '%') continue;
            if (n < cap - 1) out[n++] = (char)c; else overflow = true;
        }
        out[n] = 0;
        if (overflow) { strcpy(out, "!OVERFLOW"); return true; }
        if (n) return true;
    }
    return false;
}

void CncModule::streamJob()
{
    if (!_job.active || _stopping) return;
    while (true) {
        if (!_job.haveNext) {
            if (_job.eof) break;
            if (!readJobLine(_job.next, 81)) { _job.eof = true; break; }
            if (strcmp(_job.next, "!OVERFLOW") == 0) {
                message(2, "Line %lu too long (>80)", (unsigned long)(_job.lines + 1));
                sendEvent(CNC_EV_ERROR, 0);
                stop();
                return;
            }
            _job.haveNext = true;
        }
        if (!_grbl->queueLine(_job.next, Grbl::TAG_JOB)) break;
        _job.haveNext = false;
        _job.lines++;
    }
    // done when every line is acknowledged and the machine has actually come to rest
    const Grbl::Status &st = _grbl->status();
    static uint32_t idleSince = 0;
    if (_job.eof && !_job.haveNext && _grbl->inFlight(Grbl::TAG_JOB) == 0 &&
        (st.state == Grbl::IDLE || st.state == Grbl::CHECK)) {
        if (!idleSince) idleSince = millis();
        if (millis() - idleSince > 2 * GRBL_POLL_FAST_MS) { idleSince = 0; finishJob(CNC_EV_DONE, 0); }
    } else {
        idleSince = 0;
    }
}

void CncModule::finishJob(uint8_t ev, uint16_t arg)
{
    if (_job.f) _job.f.close();
    bool was = _job.active;
    _job.active = false;
    _job.haveNext = false;
    if (!was) return;
    sendEvent(ev, arg);
    if (ev == CNC_EV_DONE) message(0, "Done: %s (%lu lines)", _job.name.c_str(), (unsigned long)_job.lines);
    sendStatus(true);
}

uint16_t CncModule::progress() const
{
    if (!_job.active || !_job.size) return 0;
    uint32_t pos = const_cast<File &>(_job.f).position();
    return (uint16_t)((uint64_t)pos * 1000 / _job.size);
}

void CncModule::stop()
{
    jogCancel();
    _ulCount = 0;
    _grbl->realtime('!');      // always: polled status may be stale; '!' when idle is a no-op
    _stopping = true;
    _stopStart = millis();
    sendStatus(true);
}

// ---- GRBL -> module -----------------------------------------------------------------------

void CncModule::onResponse(Grbl::Tag tag, int err)
{
    if (!err) return;
    if (tag == Grbl::TAG_JOB) {
        message(2, "error:%d %s (line %lu)", err, grblError(err), (unsigned long)_job.lines);
        sendEvent(CNC_EV_ERROR, (uint16_t)err);
        if (_job.active) stop();
    } else if (tag == Grbl::TAG_JOG) {
        if (err == 15) message(1, "Jog would exceed travel");
        else message(2, "Jog error:%d %s", err, grblError(err));
        jogCancel();
    } else {
        message(2, "error:%d %s", err, grblError(err));
    }
}

void CncModule::onAlarm(int code)
{
    message(2, "ALARM:%d %s", code, grblAlarm(code));
    _jogCont = false;
    _ulCount = 0;
    if (_job.active) finishJob(CNC_EV_ALARM, (uint16_t)code);
    else sendEvent(CNC_EV_ALARM, (uint16_t)code);
}

void CncModule::onText(const char *line)
{
    if (strncmp(line, "[MSG:", 5) == 0) {
        char buf[64];
        strncpy(buf, line + 5, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        char *e = strchr(buf, ']');
        if (e) *e = 0;
        message(0, "%s", buf);
    } else if (strncmp(line, "Grbl ", 5) == 0) {
        message(0, "%.40s", line);
    } else if (strncmp(line, "[PRB:", 5) == 0) {
        message(0, "Probe %s", strchr(line, ':') && line[strlen(line) - 2] == '1' ? "OK" : "FAILED");
    }
}

// ---- user lines & macros ---------------------------------------------------------------------

void CncModule::queueUserLines(const char *lines)
{
    String all(lines);
    int start = 0;
    while (start < (int)all.length()) {
        int nl = all.indexOf('\n', start);
        if (nl < 0) nl = all.length();
        String one = all.substring(start, nl);
        one.trim();
        if (one.length() && _ulCount < ULQ) {
            _ul[(_ulHead + _ulCount) % ULQ] = one;
            _ulCount++;
        }
        start = nl + 1;
    }
}

void CncModule::drainUserLines()
{
    while (_ulCount && _grbl->queueLine(_ul[_ulHead].c_str(), Grbl::TAG_USER)) {
        _ul[_ulHead] = String();
        _ulHead = (_ulHead + 1) % ULQ;
        _ulCount--;
    }
}

// ---- lists ---------------------------------------------------------------------------

void CncModule::sendList(uint8_t kind)
{
    if (kind == CNC_LIST_FILES) {
        _fileCount = 0;
#if defined(ESP8266)
        Dir d = LittleFS.openDir(GCODE_DIR);
        while (d.next() && _fileCount < MAX_FILES)
            if (d.isFile()) _files[_fileCount++] = d.fileName();
#else
        File root = LittleFS.open(GCODE_DIR);
        for (File f = root.openNextFile(); f && _fileCount < MAX_FILES; f = root.openNextFile())
            if (!f.isDirectory()) _files[_fileCount++] = String(f.name()).substring(String(f.name()).lastIndexOf('/') + 1);
#endif
        // insertion sort, case-insensitive
        for (uint8_t i = 1; i < _fileCount; i++)
            for (uint8_t j = i; j > 0 && _files[j].compareTo(_files[j - 1]) < 0; j--) {
                String t = _files[j]; _files[j] = _files[j - 1]; _files[j - 1] = t;
            }
    } else if (kind != CNC_LIST_MACROS) {
        return;
    }
    s_listKind = kind;
    s_listIdx = 0;
}

void CncModule::runListItem(uint8_t kind, uint8_t index)
{
    if (kind == CNC_LIST_FILES) {
        if (index >= _fileCount) return;
        startJob(String(GCODE_DIR "/") + _files[index]);
        return;
    }
    if (kind == CNC_LIST_MACROS) {
        if (index >= MACRO_COUNT) return;
        uint8_t s = _grbl->status().state;
        if (_job.active || _stopping || !(s == Grbl::IDLE || s == Grbl::ALARM || s == Grbl::CHECK)) {
            message(2, "Macro refused: machine %s", Grbl::stateName(s));
            sendEvent(CNC_EV_REFUSED, 0);
            return;
        }
        queueUserLines(MACROS[index].gcode);
        message(0, "%s", MACROS[index].name);
    }
}

// ---- module -> GBA ---------------------------------------------------------------------

void CncModule::sendStatus(bool force)
{
    const Grbl::Status &st = _grbl->status();
    uint32_t now = millis();
    bool fast = _job.active || st.state == Grbl::RUN || st.state == Grbl::JOG;
    uint32_t every = fast ? 100 : 250;
    if (!force && st.state == _lastState && now - _lastStatusTx < every) return;
    if (!force && !fast && st.updatedMs == _lastStatusSrc && st.state == _lastState) return;   // nothing new
    _lastStatusSrc = st.updatedMs;
    if (!_link->online()) return;

    uint8_t b[CNC_STATUS_LEN];
    b[0] = st.state;
    b[1] = st.sub;
    for (int i = 0; i < 3; i++) {
        lb_put32(b + 2 + 4 * i, (uint32_t)st.mpos[i]);
        lb_put32(b + 14 + 4 * i, (uint32_t)st.wpos[i]);
    }
    lb_put16(b + 26, st.feed);
    lb_put16(b + 28, st.spindle);
    uint8_t fl = 0;
    if (_job.active) fl |= CNC_FL_JOB_ACTIVE;
    if (_stopping) fl |= CNC_FL_STOPPING;
#if defined(ESP8266)
    if (WiFi.status() == WL_CONNECTED || WiFi.getMode() & WIFI_AP) fl |= CNC_FL_WIFI;
#endif
    b[30] = fl;
    b[31] = 0;
    lb_put16(b + 32, progress());
    b[34] = st.ovFeed;
    b[35] = st.ovRapid;
    b[36] = st.ovSpindle;
    if (_link->send(CNC_STATUS, b, sizeof(b))) {
        _lastStatusTx = now;
        _lastState = st.state;
    }
}

void CncModule::sendEvent(uint8_t ev, uint16_t arg)
{
    uint8_t b[3] = { ev, (uint8_t)arg, (uint8_t)(arg >> 8) };
    _link->send(CNC_JOB_EVENT, b, 3);
}

void CncModule::message(uint8_t level, const char *fmt, ...)
{
    char buf[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    _lastMsg = buf;
    Serial1.printf("[cnc] %s\n", buf);
    _link->sendLog(level, buf);
}
