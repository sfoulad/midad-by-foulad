#pragma once

#include <vector>

#include "SettingsList.h"

// The five settings categories, extracted from SettingsActivity::rebuildSettingsLists()
// so a second (touch) presentation can reuse the exact same placement rules --
// including the ones that aren't a plain category-field filter: the
// pwrBtnFootnoteBack visibility gate, the Login/Logout single-slot swap, the
// Browse Files/File Transfer pin-to-top, and the Arabic Font insertion order.
// SettingsActivity's own rebuildSettingsLists() now just calls this and assigns
// the result to its member vectors -- no behavior moved, only relocated.
struct CategorizedSettings {
  std::vector<SettingInfo> display;
  std::vector<SettingInfo> reader;
  std::vector<SettingInfo> controls;
  std::vector<SettingInfo> apps;
  std::vector<SettingInfo> system;
};

CategorizedSettings buildCategorizedSettings();
