#pragma once

#include <I18n.h>

#include <cstdint>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// The Apps screen, reached from the Home menu's Apps entry.
//
// Exists because the apps were effectively undiscoverable. Each one was off by
// default and, once enabled from Settings -> Apps, appeared as a synthetic tile
// inside My Books -- so finding the Pomodoro required already knowing it existed,
// then looking for it in a book library. Reported as "where are the apps?" more
// than once; this gives them one place that is theirs.
//
// Every app is listed whether or not it is enabled, which is the part that
// actually fixes discovery: opening a disabled one enables it and goes straight
// in, so the Settings toggles become "hide this" rather than the only route to
// finding anything. Files leads, since browsing the SD card is the most-used
// entry here and it lost its own Home slot to this screen.
class AppsActivity final : public Activity {
 public:
  AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Apps", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Identity rather than a path sentinel. My Books had to use fake paths
  // ("/Pomodoro") because it was a list of books and an app had to pass for one;
  // a screen that is only ever apps can just say which app it is.
  enum class AppId : uint8_t { Files, Quran, Games, Tasbih, News, Stopwatch, Pomodoro, Gym };

  struct AppEntry {
    AppId id;
    StrId label;
    // The Settings -> Apps toggle behind this app, or nullptr for the ones that
    // have none (Files is always available). Opening an app whose flag is off
    // turns it on first -- see launch().
    uint8_t CrossPointSettings::* enableFlag;
  };

  static const std::vector<AppEntry>& entries();

  // Opens the selected app, enabling it first if its toggle is off. Returns
  // false when the app could not be made available (only the Quran, whose
  // extraction needs a writable SD card).
  bool launch(const AppEntry& entry);

  int selectorIndex = 0;
  int columns = 1;
  int rowsPerPage = 1;
  ButtonNavigator buttonNavigator;
};
