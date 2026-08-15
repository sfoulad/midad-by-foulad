#pragma once
#include <functional>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  // Last periodic anti-drift redraw; see the idle-whitening block in loop().
  unsigned long lastIdleWhitenMs = 0;
  // Set by loop()'s idle timer; makes the next render full-drive the panel
  // (HALF refresh) to clear accumulated e-ink fade instead of a FAST pass
  // that skips unchanged pixels. Cleared in render().
  bool idleWhitenPending = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;

  // Holding Confirm anywhere on Home opens BluetoothActivity -- BLE-R2's second entry
  // point alongside Apps' "Midad BLE" tile, both landing on the exact same screen
  // (see BluetoothActivity's own header comment). This is only a navigation
  // shortcut: Home itself never touches BLE directly, only BluetoothActivity's
  // onEnter()/onExit() do. Confirmed free/unused on this screen before adding (grep
  // across HomeActivity.cpp found no existing getHeldTime()/hold logic). Same
  // threshold and fired-flag-swallows-the-release pattern as RecentBooksActivity's
  // long-press-to-remove (RecentBooksActivity.cpp).
  static constexpr unsigned long kBleLongPressMs = 1000;
  bool bleLongPressFired = false;

  // Convert HomeMenuItem to menu index (used in onEnter). Order matches render()'s
  // menuItems construction: eBooks, Stats, Files, Settings ("Continue Reading" isn't
  // a HomeMenuItem -- it's a prepended label tied to the recentBooks selection range,
  // handled separately in loop()). Every slot is one fixed destination; the pair that
  // used to swap on Foulad eBooks login state no longer does.
  static int menuItemToIndex(HomeMenuItem item) {
    int i = 0;
    if (item == HomeMenuItem::FOULAD_EBOOKS) return i;
    ++i;
    if (item == HomeMenuItem::STATS) return i;
    ++i;
    if (item == HomeMenuItem::APPS) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FOULAD_EBOOKS;
    if (idx == i++) return HomeMenuItem::STATS;
    if (idx == i++) return HomeMenuItem::APPS;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onRecentsOpen();
  void onStatsOpen();
  void onSettingsOpen();
  void onFouladEbooksOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
