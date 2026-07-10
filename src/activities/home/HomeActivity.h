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

  // Convert HomeMenuItem to menu index (used in onEnter). Order matches render()'s
  // menuItems construction: Foulad eBooks, Recent Books, Check for Update, Settings
  // ("Continue Reading" isn't a HomeMenuItem -- it's a prepended label tied to the
  // recentBooks selection range, handled separately in loop()).
  static int menuItemToIndex(HomeMenuItem item) {
    int i = 0;
    // Slot 0 shows eBooks (logged in) or Files (logged out) -- same index.
    if (item == HomeMenuItem::FOULAD_EBOOKS || item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::STATS) return i;
    ++i;
    if (item == HomeMenuItem::CHECK_UPDATE) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FOULAD_EBOOKS;
    if (idx == i++) return HomeMenuItem::STATS;
    if (idx == i++) return HomeMenuItem::CHECK_UPDATE;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onRecentsOpen();
  void onStatsOpen();
  void onSettingsOpen();
  void onFouladEbooksOpen();
  void onCheckUpdateOpen();

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
};
