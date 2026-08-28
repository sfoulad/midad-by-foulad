#pragma once

#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "QuranBook.h"
#include "SettingsList.h"
#include "components/UITheme.h"

// Flips a TOGGLE setting, handling the one cross-cutting special case (Quran
// extraction on first enable), for the touch Settings presentation.
// SettingsActivity.cpp has its own independent inline copy of this logic --
// this file is consumed only by the Midad-owned touch Settings screens, not
// shared with or called from SettingsActivity.cpp. Returns true if the
// caller should repaint immediately (the Quran extraction popup was shown
// and needs to be cleared).
inline bool applySettingToggle(const SettingInfo& setting, GfxRenderer& renderer) {
  if (setting.valuePtr != nullptr) {
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
    return false;
  }
  if (!setting.valueGetter || !setting.valueSetter) return false;
  const uint8_t currentValue = setting.valueGetter();
  setting.valueSetter(currentValue ? 0 : 1);
  if (setting.nameId == StrId::STR_QURAN && !currentValue) {
    // Enabling the Quran extracts the firmware-embedded EPUB to the SD card
    // (a few seconds on first enable; instant when already extracted).
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    if (!QuranBook::ensureExtracted()) {
      setting.valueSetter(0);  // no SD / write failure: stay honest, keep it off
    }
    return true;
  }
  return false;
}

// Localized "current value" text for a setting's list row, for the touch
// Settings presentation. Mirrors (but does not share code with)
// SettingsActivity's own inline render-time formatting.
inline std::string formatSettingValueText(const SettingInfo& setting) {
  std::string valueText;
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    const bool value = SETTINGS.*(setting.valuePtr);
    valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  } else if (setting.type == SettingType::TOGGLE && setting.valueGetter) {
    const uint8_t value = setting.valueGetter();
    valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t value = SETTINGS.*(setting.valuePtr);
    valueText = I18N.get(setting.enumValues[value]);
  } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
    const uint8_t value = setting.valueGetter();
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      valueText = setting.enumStringValues[value];
    } else if (value < setting.enumValues.size()) {
      valueText = I18N.get(setting.enumValues[value]);
    }
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
      char valueBuffer[32];
      if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
        valueText = tr(STR_SLEEP_NEVER);
      } else {
        snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                 static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
        valueText = valueBuffer;
      }
    } else {
      valueText = std::to_string(SETTINGS.*(setting.valuePtr));
    }
  } else if (setting.type == SettingType::VALUE && setting.valueGetter) {
    valueText = std::to_string(setting.valueGetter());
  }
  return valueText;
}
