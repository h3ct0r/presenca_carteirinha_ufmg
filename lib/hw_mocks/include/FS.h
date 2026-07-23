#pragma once

// Mock of the Arduino FS layer, backed by the in-memory store in mock_sd.cpp.
// Only the surface the project actually uses is implemented.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

class File {
 public:
    File() = default;

    explicit operator bool() const { return valid_; }
    size_t size() const { return content_.size(); }
    const char* name() const { return name_.c_str(); }
    bool isDirectory() const { return is_dir_; }

    // Readers (also satisfy ArduinoJson's custom-reader concept).
    int read();
    size_t readBytes(char* buf, size_t len);

    size_t write(const uint8_t* buf, size_t len);
    size_t write(uint8_t b);  // ArduinoJson serializer writes byte-by-byte
    void close() {}

    File openNextFile();

    // Internal constructors used by the SD mock.
    static File make_file(const std::string& path, std::string content, bool writable);
    static File make_dir(const std::string& path, std::vector<std::string> children);

 private:
    bool valid_ = false;
    bool is_dir_ = false;
    bool writable_ = false;
    std::string path_;
    std::string name_;
    std::string content_;  // read snapshot taken at open()
    size_t pos_ = 0;
    std::vector<std::string> children_;  // full paths, immediate children
    size_t child_cursor_ = 0;
};
