// Minimal Arduino shim for host-side testing of the ESP module code. Not a full Arduino.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <algorithm>

#define IRAM_ATTR
#define PROGMEM
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2

// ---- simulated time & pins (implemented by the harness) ----
uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void yield();
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t v);
int  digitalRead(uint8_t pin);
inline void noInterrupts() {}
inline void interrupts() {}

// ---- String ----
class String {
public:
    String() {}
    String(const char *s) : s_(s ? s : "") {}
    String(const std::string &s) : s_(s) {}
    String(char c) : s_(1, c) {}
    String(int v) : s_(std::to_string(v)) {}
    String(unsigned v) : s_(std::to_string(v)) {}
    String(long v) : s_(std::to_string(v)) {}
    String(unsigned long v) : s_(std::to_string(v)) {}
    const char *c_str() const { return s_.c_str(); }
    unsigned length() const { return (unsigned)s_.size(); }
    String substring(int a) const { return a >= (int)s_.size() ? String() : String(s_.substr(a)); }
    String substring(int a, int b) const { return a >= (int)s_.size() ? String() : String(s_.substr(a, b - a)); }
    int indexOf(char c, int from = 0) const { auto p = s_.find(c, from); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const char *c, int from = 0) const { auto p = s_.find(c, from); return p == std::string::npos ? -1 : (int)p; }
    int lastIndexOf(char c) const { auto p = s_.rfind(c); return p == std::string::npos ? -1 : (int)p; }
    bool endsWith(const char *e) const { size_t n = strlen(e); return s_.size() >= n && s_.compare(s_.size() - n, n, e) == 0; }
    int compareTo(const String &o) const { return s_.compare(o.s_); }
    void trim() {
        size_t a = s_.find_first_not_of(" \t\r\n"), b = s_.find_last_not_of(" \t\r\n");
        s_ = a == std::string::npos ? "" : s_.substr(a, b - a + 1);
    }
    void replace(const char *a, const char *b) {
        size_t p = 0, la = strlen(a), lb = strlen(b);
        while ((p = s_.find(a, p)) != std::string::npos) { s_.replace(p, la, b); p += lb; }
    }
    String &operator+=(const String &o) { s_ += o.s_; return *this; }
    String &operator+=(const char *o) { s_ += o; return *this; }
    bool operator==(const String &o) const { return s_ == o.s_; }
    bool operator==(const char *o) const { return s_ == o; }
    friend String operator+(const String &a, const String &b) { return String(a.s_ + b.s_); }
    friend String operator+(const String &a, const char *b) { return String(a.s_ + b); }
    friend String operator+(const char *a, const String &b) { return String(std::string(a) + b.s_); }
    const std::string &std() const { return s_; }
private:
    std::string s_;
};

// ---- Stream / Print ----
class Print {
public:
    virtual size_t write(uint8_t b) = 0;
    virtual size_t write(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) write(b[i]); return n; }
    size_t print(const char *s) { return write((const uint8_t *)s, strlen(s)); }
    size_t println(const char *s) { size_t n = print(s); write('\n'); return n + 1; }
    size_t printf(const char *fmt, ...) {
        char buf[256]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        return write((const uint8_t *)buf, n < 0 ? 0 : std::min((size_t)n, sizeof(buf) - 1));
    }
    virtual ~Print() {}
};
class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
};

class LogSerial : public Stream {   // Serial1: debug log
public:
    bool echo = false;
    size_t write(uint8_t b) override { if (echo) fputc(b, stdout); return 1; }
    int available() override { return 0; }
    int read() override { return -1; }
    void begin(unsigned long) {}
};
extern LogSerial Serial1;
