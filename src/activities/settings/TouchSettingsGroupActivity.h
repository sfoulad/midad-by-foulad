#pragma once

// Touch-first Settings group detail list for X4 Pro -- level 2 of the
// two-level touch Settings presentation (see TouchSettingsActivity.h). Shows
// the settings folded into one TouchSettingsGroup and dispatches taps by
// SettingType, reusing the exact same data/persistence/actions as
// SettingsActivity:
//   TOGGLE  -> applySettingToggle() (SettingsList.h), flips in place
//   ENUM    -> TouchOptionPickerActivity (tap-to-select full list)
//   VALUE   -> IntervalSelectionActivity (already touch-native drag slider)
//   ACTION  -> dispatchSettingAction() (SettingsActionDispatch.h)
#ifndef SIMULATOR
#include <BoardConfig.h>
#endif

#if FREEINK_CAP_TOUCH

#include <vector>

#include "SettingsGroupMap.h"
#include "activities/UiListActivity.h"

class TouchSettingsGroupActivity final : public UiListActivity {
  TouchSettingsGroup group;
  std::vector<SettingInfo> settings;
  std::vector<std::string> rowValueStrings;
  std::vector<freeink::ui::ListItem> rowItems;

  // Display & lighting only: a synthetic row (no backing SettingInfo) opening
  // the existing gesture-triggered FrontlightPanelActivity, appended after
  // settings.size() real rows -- see rebuildRowItems()/activateIndex().
  bool hasFrontlightRow = false;

  void rebuildRowItems();
  void editEnum(int index);
  void editValue(int index);
  void openFrontlightPanel();

 public:
  TouchSettingsGroupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, TouchSettingsGroup group);
  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return static_cast<int>(rowItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
};

#endif  // FREEINK_CAP_TOUCH
