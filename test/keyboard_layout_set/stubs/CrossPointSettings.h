#pragma once

#include <cstdint>

// KeyboardLayoutSet reads exactly one field. Everything else in the real
// settings object is unrelated persistence.
class CrossPointSettings {
 public:
  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
  uint16_t keyboardLayouts = 0;
};

#define SETTINGS CrossPointSettings::getInstance()
