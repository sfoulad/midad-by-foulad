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
    TYPE_WORD,
    LOOKUP_HISTORY,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    MIDAD_SYNC,
    DELETE_CACHE,
    // Start / stop / acknowledge the reading Pomodoro (see ReaderPomodoro.h). The
    // drawer only reports the press; the reader owns the session so the footer and the
    // end-of-phase flash stay with whoever is painting the page.
    POMODORO,
    // Handled inside the drawer (never returned as a result):
    FONT_SIZE,
    FONT_NAME,
    TEXT_ALIGN,
    LINE_SPACING,
    RESET_BOOK_SETTINGS,
    NIGHT_MODE,
    FRONTLIGHT,
    // Opens the installed-dictionary list as a drill-down; picking a row calls
    // DICTIONARIES.setActiveIndex() and returns to the Dictionary tab.
    ACTIVE_DICTIONARY,
    DEFINITION_TEXT_SIZE
  };

  // `epub` is non-owning: the reader keeps the Epub alive for the whole time
  // this (child) activity exists. Used for the in-drawer TOC list.
  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Epub* epub,
                                  const int currentSpineIndex, const int currentPage, const int totalPages,
                                  const int bookProgressPercent, const uint8_t currentOrientation,
                                  const bool hasFootnotes, bool hasBookmarks, bool isArabicBook, bool canSyncMidad);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // READING and SETTINGS are the two top-level icon tabs; CHAPTERS is a
  // drill-down reached from a row in the Reading tab (Back returns to
  // READING, not to a "MAIN" state -- there is no third tab).
  // Four top-level tabs, drawn as text labels through GUI.drawTabBar (the same
  // helper Settings uses for its five categories) rather than as a hand-laid icon
  // row. DICTIONARY is dropped from the strip entirely when no dictionary is
  // installed, so the tab count is 3 or 4 at runtime -- another reason the strip is
  // built as a vector rather than positioned by fixed fractions of the width.
  //
  // CHAPTERS and DICTIONARY_LIST are drill-downs, not tabs: each is reached from a
  // row, hides the tab strip while open, and Back returns to the tab it came from
  // rather than closing the drawer.
  enum class View : uint8_t { DICTIONARY, TEXT, NAVIGATE, SETTINGS_TAB, CHAPTERS, DICTIONARY_LIST };

  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  std::vector<MenuItem> buildDictionaryItems() const;
  std::vector<MenuItem> buildTextItems() const;
  std::vector<MenuItem> buildNavigateItems(bool hasBookmarks, bool hasFootnotes) const;
  std::vector<MenuItem> buildSettingsItems() const;

  // The tabs actually on the strip, in display order. Rebuilt with the item lists,
  // never recomputed piecemeal -- the strip, the cycle order and the tab-to-item
  // mapping all read from this one vector so they cannot disagree.
  std::vector<View> tabOrder;
  int tabPosition(View v) const;
  const char* tabLabel(View v) const;
  View nextTab() const;
  // False when THIS book has no catalog id -- side-loaded, or downloaded before
  // the id was recorded. The row is hidden rather than shown-and-inert: pressing
  // Sync and having nothing at all happen is worse than not offering it.
  bool canSyncMidad_ = false;

  std::string valueLabel(MenuAction action) const;
  std::string globalLabel(const char* effectiveValueLabel) const;
  void openSettingEditor(MenuAction action);
  void finishWithAction(int action, bool cancelled);
  void handleListConfirm();

  const std::vector<MenuItem>& activeItems() const {
    switch (view) {
      case View::TEXT:
        return textItems;
      case View::NAVIGATE:
        return navigateItems;
      case View::SETTINGS_TAB:
        return settingsItems;
      default:
        return dictionaryItems;
    }
  }
  int& activeIndex() {
    switch (view) {
      case View::TEXT:
        return textSelectedIndex;
      case View::NAVIGATE:
        return navigateSelectedIndex;
      case View::SETTINGS_TAB:
        return settingsSelectedIndex;
      case View::CHAPTERS:
        return chapterSelectedIndex;
      case View::DICTIONARY_LIST:
        return dictionarySelectedIndex;
      default:
        return selectedIndex;
    }
  }
  int activeItemCount() const;

  Epub* const epub;  // non-owning (see ctor note)
  const int currentSpineIndex;

  // Fixed menu layout
  std::vector<MenuItem> dictionaryItems;
  std::vector<MenuItem> textItems;
  std::vector<MenuItem> navigateItems;
  std::vector<MenuItem> settingsItems;
  View view = View::DICTIONARY;
  // Starts on the tab row (matching Settings' own default focus position, per
  // user request), not the first list item.
  int selectedIndex = -1;
  int textSelectedIndex = 0;
  int navigateSelectedIndex = 0;
  int settingsSelectedIndex = 0;
  int chapterSelectedIndex = 0;
  int dictionarySelectedIndex = 0;
  // The tab each drill-down was opened from, so Back returns there. Chapters is
  // only reachable from Navigate today, but storing it costs a byte and stops the
  // return path from being a second place that has to know which tab owns the row.
  View dictionaryListReturnTo = View::DICTIONARY;
  View chapterReturnTo = View::NAVIGATE;

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
  // Built from DictionaryStore's own enum rather than spelled out here: the popup
  // reports the picked index straight through to setDefinitionTextSize(), so a list
  // shorter than the enum makes the missing option unreachable and mislabels it in
  // the value column. Filled in the constructor, once the strings are loaded.
  std::vector<const char*> definitionTextSizeLabels;
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
};
