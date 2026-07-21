#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// In-book menu, rendered as a bottom drawer (~85% of the screen, live page
// visible above). Two icon tabs at the top switch between the Reading list
// (most-used actions plus the per-book reading settings -- font size/name,
// line spacing, alignment -- script-aware: an Arabic book shows only the
// Arabic font rows, a Latin book only the English ones) and the Settings list
// (everything else). The chapter list opens as a drill-down from the Reading
// tab (no separate activity); Back from it returns to the Reading tab rather
// than exiting the drawer. Setting rows edit the RAM-only SETTINGS.book*
// overrides in place (see CrossPointSettings.h) and are reported back via
// MenuResult::bookSettingsChanged so the reader persists the sidecar and
// re-lays-out on close.
class EpubReaderMenuActivity final : public Activity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    // Returned to the reader through MenuResult:
    SELECT_CHAPTER,  // returned with chapterSpineIndex/chapterAnchor already picked
    FOOTNOTES,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    BOOKMARKS,
    TOGGLE_BOOKMARK,
    LOOKUP_WORD,
    LOOKUP_HISTORY,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    // Handled inside the drawer (never returned as a result):
    FONT_SIZE,
    FONT_NAME,
    TEXT_ALIGN,
    LINE_SPACING,
    RESET_BOOK_SETTINGS
  };

  // `epub` is non-owning: the reader keeps the Epub alive for the whole time
  // this (child) activity exists. Used for the in-drawer TOC list.
  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Epub* epub,
                                  const int currentSpineIndex, const int currentPage, const int totalPages,
                                  const int bookProgressPercent, const uint8_t currentOrientation,
                                  const bool hasFootnotes, bool hasBookmarks, bool isArabicBook);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // READING and SETTINGS are the two top-level icon tabs; CHAPTERS is a
  // drill-down reached from a row in the Reading tab (Back returns to
  // READING, not to a "MAIN" state -- there is no third tab).
  enum class View : uint8_t { READING, SETTINGS_TAB, CHAPTERS };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  std::vector<MenuItem> buildReadingItems(bool hasBookmarks) const;
  std::vector<MenuItem> buildSettingsItems(bool hasFootnotes) const;

  std::string valueLabel(MenuAction action) const;
  std::string globalLabel(const char* effectiveValueLabel) const;
  void openSettingEditor(MenuAction action);
  void finishWithAction(int action, bool cancelled);
  void handleListConfirm();

  const std::vector<MenuItem>& activeItems() const {
    return view == View::SETTINGS_TAB ? settingsItems : readingItems;
  }
  int& activeIndex() {
    switch (view) {
      case View::SETTINGS_TAB:
        return settingsSelectedIndex;
      case View::CHAPTERS:
        return chapterSelectedIndex;
      default:
        return selectedIndex;
    }
  }
  int activeItemCount() const;

  Epub* const epub;  // non-owning (see ctor note)
  const int currentSpineIndex;

  // Fixed menu layout
  std::vector<MenuItem> readingItems;
  std::vector<MenuItem> settingsItems;
  View view = View::READING;
  // Starts on the tab row (matching Settings' own default focus position, per
  // user request), not the first list item.
  int selectedIndex = -1;
  int settingsSelectedIndex = 0;
  int chapterSelectedIndex = 0;

  const bool isArabicBook;
  bool bookSettingsChanged = false;
  // SD families for the book's script (drives the Font Name row; empty = row hidden).
  std::vector<std::string> sdFamilies;

  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
