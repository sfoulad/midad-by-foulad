#pragma once

// Host-test stand-in for lib/hal/HalStorage.h, providing only the slice of the
// HalFile / Storage surface that Bitmap.cpp touches, backed by plain stdio.
// Placed FIRST on the include path (see this directory's CMakeLists.txt) so the
// real, FreeRTOS/SdFat-bound header never gets pulled into the host build --
// which lets the tests exercise the ACTUAL BMP header parser the firmware runs,
// rather than a re-implementation that could drift away from it.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::FILE* handle) : handle_(handle) {}
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  HalFile& operator=(HalFile&& other) noexcept {
    if (this != &other) {
      close();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  ~HalFile() { close(); }

  explicit operator bool() const { return handle_ != nullptr; }

  bool seek(size_t pos) { return handle_ != nullptr && std::fseek(handle_, static_cast<long>(pos), SEEK_SET) == 0; }
  bool seekCur(int64_t offset) {
    return handle_ != nullptr && std::fseek(handle_, static_cast<long>(offset), SEEK_CUR) == 0;
  }

  int read() {
    if (handle_ == nullptr) return -1;
    const int c = std::fgetc(handle_);
    return c == EOF ? -1 : c;
  }

  int read(void* buf, size_t count) {
    if (handle_ == nullptr) return -1;
    return static_cast<int>(std::fread(buf, 1, count, handle_));
  }

  bool close() {
    if (handle_ == nullptr) return false;
    std::fclose(handle_);
    handle_ = nullptr;
    return true;
  }

 private:
  std::FILE* handle_ = nullptr;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    file = HalFile(handle);
    return handle != nullptr;
  }
};

#define Storage HalStorage::getInstance()
