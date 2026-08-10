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
  // Independent cache slots -- hero cover + one per thumbnail (FouladTheme's own
  // kThumbsCount, currently 3) -- instead of one combined ~42KB buffer, see
  // BaseTheme.h's drawRecentBookCover comment. Each slot's malloc is small enough
  // (a few KB) to succeed on its own even when BlePeripheralManager or similar is
  // holding a large chunk of heap. Live-debugged 2026-08-10 in two rounds: the
  // original single 40+KB buffer reliably failed and forced a full SD re-decode
  // of every cover on every selector move; splitting into 2 slots (hero + one
  // combined thumbnail row) still left the 14KB thumbnail-row slot failing under
  // real measured ~15KB-free BLE pressure, so it's split further into one slot
  // per thumbnail (~4.8KB each) so a squeeze only costs whichever thumbnail(s)
  // actually don't fit.
  static constexpr int kCoverSlotCount = 4;
  struct CoverSlot {
    uint8_t* buffer = nullptr;
    size_t bufferSize = 0;
    bool stored = false;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
  };
  CoverSlot coverSlots[kCoverSlotCount];
  // Holding Confirm anywhere on Home opens BluetoothActivity (see its own header
  // comment for why this is BLE's only entry point) -- confirmed free/unused on
  // this screen before adding (grep across HomeActivity.cpp found no existing
  // getHeldTime()/hold logic). Same threshold and fired-flag-swallows-the-release
  // pattern as RecentBooksActivity's long-press-to-remove.
  static constexpr unsigned long kBleLongPressMs = 1000;
  bool bleLongPressFired = false;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;

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
  bool storeCoverSlot(int slot, int x, int y, int w, int h);  // Snapshot a region into the given slot
  bool restoreCoverSlot(int slot);                            // Blit a slot's snapshot back, if it has one
  void freeCoverSlots();                                      // Free every slot's buffer
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
};
