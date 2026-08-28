#pragma once

// Generic full-screen "pick one of N" touch list -- the touch-native
// equivalent of OptionPopup (which stays button-only; X3/X4 keep using it
// unchanged). Reused by every ENUM setting in the touch Settings presentation
// instead of each screen building its own picker. Tap a row to select and
// finish(); Back cancels. RTL mirroring is inherited from UiListActivity's
// shared list machinery, same as every other touch list screen.
#ifndef SIMULATOR
#include <BoardConfig.h>
#endif

#if FREEINK_CAP_TOUCH

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class TouchOptionPickerActivity final : public UiListActivity {
  std::string title;
  std::vector<std::string> options;
  int currentIndex;
  std::vector<freeink::ui::ListItem> rowItems;

 public:
  // `options` are already-localized display strings (caller resolves StrId /
  // enumStringValues before constructing -- keeps this activity oblivious to
  // where the choices came from, same reasoning as SettingInfo's own
  // enumValues/enumStringValues split).
  TouchOptionPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                            std::vector<std::string> options, int currentIndex);
  void onEnter() override;

 private:
  int listCount() const override { return static_cast<int>(options.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override { return title.c_str(); }
};

#endif  // FREEINK_CAP_TOUCH
