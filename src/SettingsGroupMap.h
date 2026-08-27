#pragma once

#include <vector>

#include "SettingsCategoryBuilder.h"
#include "SettingsList.h"

// The touch Settings presentation's 5 groups (General / Display & lighting /
// Network & Bluetooth / Reading / Device & System), built by folding the same
// CategorizedSettings vectors SettingsActivity uses -- no separate placement
// rules, so nothing here can drift from what X3/X4's button UI shows.
//
// Fold, with rationale (see the approved plan for the full table):
//   General            <- controls        (button/remap settings; the closest
//                                           fit among the 5 fixed names)
//   Display & lighting <- display
//   Network & Bluetooth<- system rows whose action == SettingAction::Network
//   Reading            <- reader
//   Device & System    <- apps + (system minus the Network row)
enum class TouchSettingsGroup : uint8_t {
  General = 0,
  DisplayLighting,
  NetworkBluetooth,
  Reading,
  DeviceSystem,
  Count,
};

struct TouchSettingsGroups {
  std::vector<SettingInfo> general;
  std::vector<SettingInfo> displayLighting;
  std::vector<SettingInfo> networkBluetooth;
  std::vector<SettingInfo> reading;
  std::vector<SettingInfo> deviceSystem;

  const std::vector<SettingInfo>& operator[](TouchSettingsGroup group) const {
    switch (group) {
      case TouchSettingsGroup::General:
        return general;
      case TouchSettingsGroup::DisplayLighting:
        return displayLighting;
      case TouchSettingsGroup::NetworkBluetooth:
        return networkBluetooth;
      case TouchSettingsGroup::Reading:
        return reading;
      case TouchSettingsGroup::DeviceSystem:
      default:
        return deviceSystem;
    }
  }
};

inline TouchSettingsGroups buildTouchSettingsGroups() {
  CategorizedSettings categorized = buildCategorizedSettings();

  TouchSettingsGroups out;
  out.general = std::move(categorized.controls);
  out.displayLighting = std::move(categorized.display);
  out.reading = std::move(categorized.reader);
  out.deviceSystem = std::move(categorized.apps);

  out.deviceSystem.reserve(out.deviceSystem.size() + categorized.system.size());
  for (auto& setting : categorized.system) {
    if (setting.action == SettingAction::Network) {
      out.networkBluetooth.push_back(std::move(setting));
    } else {
      out.deviceSystem.push_back(std::move(setting));
    }
  }
  return out;
}
