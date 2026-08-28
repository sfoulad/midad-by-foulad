#pragma once

// Touch-first Settings category list for X4 Pro (and any other
// FREEINK_CAP_TOUCH board) -- level 1 of the two-level touch Settings
// presentation. X3/X4's button-driven SettingsActivity is unchanged; see
// ActivityManager::goToSettings() for the runtime dispatch between the two.
//
// Reuses the exact same settings data model, persistence, and actions as
// SettingsActivity (via SettingsGroupMap.h / SettingsCategoryBuilder.h) --
// this file and TouchSettingsGroupActivity are presentation only.
//
// FREEINK_CAP_TOUCH is defined by BoardConfig.h (device-only SDK surface, no
// simulator equivalent -- see SettingsList.h's own comment on boardHasTouch()).
// Under SIMULATOR this include is skipped and the macro stays undefined, so
// the #if below correctly compiles this whole file out.
#ifndef SIMULATOR
#include <BoardConfig.h>
#endif

#if FREEINK_CAP_TOUCH

#include <vector>

#include "activities/UiListActivity.h"

class TouchSettingsActivity final : public UiListActivity {
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();

 public:
  explicit TouchSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;

 private:
  int listCount() const override { return static_cast<int>(rowItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
};

#endif  // FREEINK_CAP_TOUCH
