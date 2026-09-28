#pragma once

#include <cstdint>

enum class Language : uint8_t { EN = 0, AR = 1, BG = 2, FA = 3, _COUNT };

class I18n {
 public:
  static I18n& getInstance() {
    static I18n instance;
    return instance;
  }
  Language getLanguage() const { return language; }
  void setLanguage(const Language value) { language = value; }

 private:
  Language language = Language::EN;
};

#define I18N I18n::getInstance()
