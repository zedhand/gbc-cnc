// In-memory filesystem shim (just what the module code uses).
#pragma once
#include "Arduino.h"
#include <map>
#include <memory>

namespace fs {

struct MemFS { std::map<std::string, std::string> files; };

class File {
public:
    File() {}
    File(MemFS *fs, const std::string &path, bool write, bool dir)
        : fs_(fs), path_(path), write_(write), dir_(dir), ok_(true) {}
    explicit operator bool() const { return ok_; }
    size_t size() const { return dir_ || !ok_ ? 0 : fs_->files[path_].size(); }
    size_t position() const { return pos_; }
    bool seek(uint32_t p) { if (p > size()) return false; pos_ = p; return true; }
    int available() { return ok_ && !dir_ ? (int)(size() - pos_) : 0; }
    int read() { if (available() <= 0) return -1; return (uint8_t)fs_->files[path_][pos_++]; }
    size_t read(uint8_t *b, size_t n) { size_t k = 0; while (k < n && available() > 0) b[k++] = (uint8_t)read(); return k; }
    size_t write(const uint8_t *b, size_t n) { if (!write_) return 0; fs_->files[path_].append((const char *)b, n); return n; }
    void close() { ok_ = false; }
    bool isDirectory() const { return dir_; }
    const char *name() const { name_ = path_.substr(path_.rfind('/') + 1); return name_.c_str(); }
    File openNextFile() {
        std::string prefix = path_ + "/";
        for (auto it = fs_->files.upper_bound(last_); it != fs_->files.end(); ++it) {
            if (it->first.compare(0, prefix.size(), prefix) == 0 && it->first.find('/', prefix.size()) == std::string::npos) {
                last_ = it->first;
                return File(fs_, it->first, false, false);
            }
        }
        return File();
    }
private:
    MemFS *fs_ = nullptr;
    std::string path_;
    mutable std::string name_;
    std::string last_;
    bool write_ = false, dir_ = false, ok_ = false;
    size_t pos_ = 0;
};

class FS {
public:
    MemFS mem;
    bool begin() { return true; }
    bool format() { mem.files.clear(); return true; }
    bool mkdir(const char *) { return true; }
    bool mkdir(const String &) { return true; }
    bool remove(const String &p) { return mem.files.erase(p.std()) > 0; }
    File open(const String &p, const char *mode = "r") { return open(p.c_str(), mode); }
    File open(const char *p, const char *mode = "r") {
        std::string s(p);
        if (mode[0] == 'w') { mem.files[s].clear(); return File(&mem, s, true, false); }
        if (mem.files.count(s)) return File(&mem, s, false, false);
        std::string pre = s + "/";
        for (auto &kv : mem.files) if (kv.first.compare(0, pre.size(), pre) == 0) return File(&mem, s, false, true);
        return File();
    }
};
}  // namespace fs
using fs::File;
