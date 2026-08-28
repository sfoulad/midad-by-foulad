#pragma once

#include <I18n.h>

#include <cstdint>
#include <functional>
#include <vector>

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
// The Settings -> Apps toggles are "hide this": switching an app off removes its
// tile from here, which is what those toggles have always claimed to do. The one
// exception is the Quran, which is off by default because enabling it extracts a
// real EPUB to the SD card -- hiding it while off would make it undiscoverable,
// exactly the problem this screen exists to solve, so its tile stays and pressing
// it is the opt-in. Files leads, since browsing the SD card is the most-used entry
// here and it lost its own Home slot to this screen.
class AppsActivity final : public Activity {
 public:
  AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Apps", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Identity rather than a path sentinel. My Books had to use fake paths
  // ("/Pomodoro") because it was a list of books and an app had to pass for one;
  // a screen that is only ever apps can just say which app it is.
  enum class AppId : uint8_t { Files, Quran, Games, Tasbih, News, Stopwatch, Pomodoro, Gym, MidadBle };

  struct AppEntry {
    AppId id;
    StrId label;
    // 32x32, GfxRenderer::drawIcon() format -- see components/icons/appLauncherIcons.h.
    const uint8_t* icon;
    // The Settings -> Apps toggle behind this app, or empty for the ones that
    // have none (Files, and MidadBle -- see below). Opening an app whose flag
    // is off turns it on first -- see launch(). Getter/setter rather than a
    // pointer-to-member since the backing fields live on different stores
    // (MidadAppSettings for most apps) -- see SettingInfo::DynamicToggle for
    // the same reasoning applied to the Settings screen.
    std::function<uint8_t()> getEnabled;
    std::function<void(uint8_t)> setEnabled;
  };

  // One page's worth of tile geometry, shared between render() (drawing) and
  // loop() (touch hit-testing) so the two can never drift apart.
  struct Geometry {
    int gridStartX;
    int contentTop;
    int tileWidth;
    int tileHeight;
    int gutter;
  };

  static const std::vector<AppEntry>& entries();

  // entries() minus the apps switched off in Settings -- what this screen shows.
  // Rebuilt per call (eight pointers, no allocation worth caching) so a toggle
  // changed in Settings takes effect the next time this screen draws. Clamps
  // selectorIndex to the result, which is why it is not const: every caller needs
  // that done before it indexes, and doing it here keeps them in step.
  std::vector<const AppEntry*> visibleEntries();

  // Opens the selected app, enabling it first if its toggle is off. Returns
  // false when the app could not be made available (only the Quran, whose
  // extraction needs a writable SD card).
  bool launch(const AppEntry& entry);

  Geometry computeGeometry() const;
  void drawAppTile(int cellX, int cellY, int tileWidth, int tileHeight, const uint8_t* icon, const std::string& label,
                   bool selected, bool rtl) const;
  void drawPagination(int centerX, int y, int currentPage, int pageCount) const;

  int selectorIndex = 0;
  ButtonNavigator buttonNavigator;

  // Remembers the selection across the destroy/reconstruct cycle Files and
  // Quran go through (activityManager.goToFileBrowser/goToReader both call
  // replaceActivity(), which deletes this object -- see the Activity Lifecycle
  // section of CLAUDE.md). The 6 other apps push via startActivityForResult()
  // instead, which never destroys this object, so their return path already
  // preserves selectorIndex for free; this static is what makes Files/Quran
  // behave the same way. Written in onExit() (every exit path reaches it) and
  // read in onEnter(), so it also just works when this object IS reused.
  static int rememberedSelectorIndex;
};
