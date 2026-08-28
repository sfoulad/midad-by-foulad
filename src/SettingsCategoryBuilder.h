#pragma once

#include <vector>

#include "SettingsList.h"

// The five settings categories, independently mirroring
// SettingsActivity::rebuildSettingsLists()'s placement rules for the touch
// Settings presentation -- including the ones that aren't a plain
// category-field filter: the pwrBtnFootnoteBack visibility gate, the
// Login/Logout single-slot swap, the Browse Files/File Transfer pin-to-top,
// and the Arabic Font insertion order. SettingsActivity.cpp is untouched and
// does not call this -- kept deliberately separate so the touch presentation
// stays isolated Midad-owned code with no shared dependency on (or from)
// SettingsActivity's own internals.
struct CategorizedSettings {
  std::vector<SettingInfo> display;
  std::vector<SettingInfo> reader;
  std::vector<SettingInfo> controls;
  std::vector<SettingInfo> apps;
  std::vector<SettingInfo> system;
};

CategorizedSettings buildCategorizedSettings();
