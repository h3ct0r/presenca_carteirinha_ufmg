#include "mock_sd.h"

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "SD_MMC.h"

MockSDMMC SD_MMC;

namespace {

struct Store {
    std::mutex m;
    std::map<std::string, std::string> files;
    std::set<std::string> dirs;
    bool begin_ok = true;
    bool mounted = false;
};

// Heap-allocated and intentionally never destroyed: detached mock tasks
// (FreeRTOS shim) may still touch the store during process teardown.
Store& S() {
    static Store* s = new Store();
    return *s;
}

std::string basename_of(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

void add_parent_dirs_locked(Store& s, const std::string& path) {
    for (size_t pos = path.find('/', 1); pos != std::string::npos;
         pos = path.find('/', pos + 1)) {
        s.dirs.insert(path.substr(0, pos));
    }
}

bool is_root(const std::string& path) { return path == "/"; }

// Immediate children (files and dirs) of a directory, unique, full paths.
std::vector<std::string> children_locked(Store& s, const std::string& dir) {
    std::set<std::string> out;
    // The root is the one directory whose path already ends in '/', so the
    // usual dir + "/" would build "//" and match nothing.
    const std::string prefix = is_root(dir) ? "/" : dir + "/";
    auto consider = [&](const std::string& path) {
        if (path.compare(0, prefix.size(), prefix) != 0) return;
        size_t next = path.find('/', prefix.size());
        out.insert(next == std::string::npos ? path : path.substr(0, next));
    };
    for (const auto& kv : s.files) consider(kv.first);
    for (const auto& d : s.dirs) consider(d);
    return std::vector<std::string>(out.begin(), out.end());
}

}  // namespace

// ---- test-control API ------------------------------------------------------

void mocksd_reset(void) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    s.files.clear();
    s.dirs.clear();
    s.begin_ok = true;
    s.mounted = false;
}

void mocksd_set_begin_result(bool ok) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    s.begin_ok = ok;
}

void mocksd_add_file(const char* path, const char* contents) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    s.files[path] = contents;
    add_parent_dirs_locked(s, path);
}

void mocksd_add_dir(const char* path) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    s.dirs.insert(path);
    add_parent_dirs_locked(s, path);
}

bool mocksd_exists(const char* path) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    return s.files.count(path) || s.dirs.count(path) || is_root(path);
}

size_t mocksd_file_size(const char* path) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    auto it = s.files.find(path);
    return it == s.files.end() ? 0 : it->second.size();
}

size_t mocksd_read_file(const char* path, void* out, size_t cap) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    auto it = s.files.find(path);
    if (it == s.files.end()) return 0;
    size_t n = it->second.size() < cap ? it->second.size() : cap;
    memcpy(out, it->second.data(), n);
    return n;
}

// ---- File ------------------------------------------------------------------

File File::make_file(const std::string& path, std::string content, bool writable) {
    File f;
    f.valid_ = true;
    f.is_dir_ = false;
    f.writable_ = writable;
    f.path_ = path;
    f.name_ = basename_of(path);
    f.content_ = std::move(content);
    return f;
}

File File::make_dir(const std::string& path, std::vector<std::string> children) {
    File f;
    f.valid_ = true;
    f.is_dir_ = true;
    f.path_ = path;
    f.name_ = basename_of(path);
    f.children_ = std::move(children);
    return f;
}

int File::read() {
    if (!valid_ || pos_ >= content_.size()) return -1;
    return (unsigned char)content_[pos_++];
}

size_t File::readBytes(char* buf, size_t len) {
    if (!valid_) return 0;
    size_t left = content_.size() - pos_;
    size_t n = len < left ? len : left;
    memcpy(buf, content_.data() + pos_, n);
    pos_ += n;
    return n;
}

size_t File::write(const uint8_t* buf, size_t len) {
    if (!valid_ || !writable_) return 0;
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    s.files[path_].append((const char*)buf, len);
    return len;
}

size_t File::write(uint8_t b) { return write(&b, 1); }

File File::openNextFile() {
    if (!valid_ || !is_dir_ || child_cursor_ >= children_.size()) return File();
    const std::string child = children_[child_cursor_++];
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    if (s.dirs.count(child)) return make_dir(child, children_locked(s, child));
    auto it = s.files.find(child);
    if (it == s.files.end()) return File();
    return make_file(child, it->second, false);
}

// ---- SD_MMC ----------------------------------------------------------------

bool MockSDMMC::begin(const char*, bool) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    if (!s.begin_ok) return false;
    s.mounted = true;
    return true;
}

bool MockSDMMC::exists(const char* path) { return mocksd_exists(path); }

File MockSDMMC::open(const char* path, const char* mode, bool) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    const bool truncate = (mode != nullptr && mode[0] == 'w');
    const bool append = (mode != nullptr && mode[0] == 'a');
    if (truncate || append) {
        if (truncate || !s.files.count(path)) s.files[path] = "";  // create; 'w' truncates
        add_parent_dirs_locked(s, path);
        return File::make_file(path, "", true);  // File::write appends to the store
    }
    // The card root always exists, even on an empty card — it is never in
    // s.dirs because nothing creates it.
    if (s.dirs.count(path) || is_root(path)) {
        return File::make_dir(path, children_locked(s, path));
    }
    auto it = s.files.find(path);
    if (it == s.files.end()) return File();
    return File::make_file(path, it->second, false);
}

bool MockSDMMC::mkdir(const char* path) {
    mocksd_add_dir(path);
    return true;
}

bool MockSDMMC::rmdir(const char* path) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    if (!s.dirs.count(path)) return false;
    if (!children_locked(s, path).empty()) return false;  // real rmdir needs it empty
    s.dirs.erase(path);
    return true;
}

bool MockSDMMC::remove(const char* path) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    return s.files.erase(path) > 0;  // files only, like the real one
}

// Renames a file, or a directory together with everything under it (FatFs
// f_rename handles both).
bool MockSDMMC::rename(const char* from, const char* to) {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    auto it = s.files.find(from);
    if (it != s.files.end()) {
        s.files[to] = it->second;
        s.files.erase(it);
        add_parent_dirs_locked(s, to);
        return true;
    }
    if (!s.dirs.count(from)) return false;

    // Re-key the subtree: <from> and every path prefixed with "<from>/".
    const std::string prefix = std::string(from) + "/";
    auto moved = [&](const std::string& p) {
        return std::string(to) + p.substr(strlen(from));
    };
    std::map<std::string, std::string> files;
    for (auto& kv : s.files) {
        files[kv.first.compare(0, prefix.size(), prefix) == 0 ? moved(kv.first) : kv.first] =
            kv.second;
    }
    std::set<std::string> dirs;
    for (const auto& d : s.dirs) {
        bool under = d == from || d.compare(0, prefix.size(), prefix) == 0;
        dirs.insert(under ? moved(d) : d);
    }
    s.files.swap(files);
    s.dirs.swap(dirs);
    add_parent_dirs_locked(s, to);
    return true;
}

uint64_t MockSDMMC::cardSize() { return 4ULL * 1024 * 1024 * 1024; }
uint64_t MockSDMMC::totalBytes() { return cardSize(); }
uint64_t MockSDMMC::usedBytes() {
    Store& s = S();
    std::lock_guard<std::mutex> lk(s.m);
    uint64_t used = 0;
    for (const auto& kv : s.files) used += kv.second.size();
    return used;
}
