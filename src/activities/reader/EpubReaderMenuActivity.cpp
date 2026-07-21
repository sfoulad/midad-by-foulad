#include "EpubReaderMenuActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "ArabicFontSystem.h"
#include "CrossPointSettings.h"
#include "DictionaryStore.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/settings2.h"
#include "fontIds.h"

namespace {

constexpr StrId kSizeLabels[CrossPointSettings::FONT_SIZE_COUNT] = {StrId::STR_SMALL, StrId::STR_MEDIUM,
                                                                    StrId::STR_LARGE, StrId::STR_X_LARGE};
constexpr StrId kSpacingLabels[CrossPointSettings::LINE_COMPRESSION_COUNT] = {StrId::STR_TIGHT, StrId::STR_NORMAL,
                                                                              StrId::STR_WIDE};
constexpr StrId kAlignLabels[CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT] = {
    StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE};
// Selectable built-in Arabic families for the per-book override picker: (label,
// ARABIC_FONT_FAMILY value) pairs, in display order. Amiri's font data was removed to
// save flash space, so it's no longer offered; its enum value (1) stays reserved (see
// CrossPointSettings::ARABIC_FONT_FAMILY) so an existing per-book sidecar still holding
// it resolves through ArabicFontSystem's Naskh alias. Display index is therefore NOT
// the same as the stored family value -- every use below translates explicitly via
// kSelectableArabicFonts / displayIndexForArabicFamily.
constexpr std::pair<StrId, uint8_t> kSelectableArabicFonts[] = {
    {StrId::STR_NOTO_NASKH_ARABIC, CrossPointSettings::NOTONASKHARABIC},
    {StrId::STR_UTHMANI_HAFS, CrossPointSettings::UTHMANICHAFS},
    {StrId::STR_TAJAWAL, CrossPointSettings::TAJAWAL},
};
constexpr int kSelectableArabicFontCount =
    static_cast<int>(sizeof(kSelectableArabicFonts) / sizeof(kSelectableArabicFonts[0]));

int displayIndexForArabicFamily(const uint8_t family) {
  for (int i = 0; i < kSelectableArabicFontCount; i++) {
    if (kSelectableArabicFonts[i].second == family) return i;
  }
  return 0;  // stale/legacy value (e.g. removed Amiri=1) or out of range
}

// Selectable built-in Latin families for the per-book override picker, mirroring
// kSelectableArabicFonts above -- (label, FONT_FAMILY value) pairs, in display order.
constexpr std::pair<StrId, uint8_t> kSelectableLatinFonts[] = {
    {StrId::STR_BITTER, CrossPointSettings::BITTER},
    {StrId::STR_LEXEND_DECA, CrossPointSettings::LEXENDDECA},
};
constexpr int kSelectableLatinFontCount =
    static_cast<int>(sizeof(kSelectableLatinFonts) / sizeof(kSelectableLatinFonts[0]));

int displayIndexForLatinFamily(const uint8_t family) {
  for (int i = 0; i < kSelectableLatinFontCount; i++) {
    if (kSelectableLatinFonts[i].second == family) return i;
  }
  return 0;  // out of range
}

std::vector<std::string> sdFamilyNames(const SdCardFontRegistry& registry) {
  std::vector<std::string> names;
  const auto& families = registry.getFamilies();
  names.reserve(families.size());
  for (const auto& f : families) names.push_back(f.name);
  return names;
}

void setBookFamily(char* field, const size_t fieldSize, const char* name) {
  strncpy(field, name, fieldSize - 1);
  field[fieldSize - 1] = '\0';
}

// Book titles can carry embedded newlines/tabs from EPUB metadata; the header
// bar is strictly one line, so flatten whitespace before truncating.
std::string flattenedTitle(const std::string& title) {
  std::string out = title;
  for (char& c : out) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  return out;
}

}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Epub* epub,
                                               const int currentSpineIndex, const int currentPage,
                                               const int totalPages, const int bookProgressPercent,
                                               const uint8_t currentOrientation, const bool hasFootnotes,
                                               const bool hasBookmarks, const bool isArabicBook)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      epub(epub),
      currentSpineIndex(currentSpineIndex),
      isArabicBook(isArabicBook),
      sdFamilies(sdFamilyNames(isArabicBook ? arabicFontSystem.registry() : sdFontSystem.registry())),
      title(epub ? flattenedTitle(epub->getTitle()) : std::string()),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {
  readingItems = buildReadingItems(hasBookmarks);
  settingsItems = buildSettingsItems(hasFootnotes);
}

// Most-used first: chapters, bookmarking, then the per-book reading settings,
// then orientation; everything else lives in the Settings tab.
std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildReadingItems(
    const bool hasBookmarks) const {
  std::vector<MenuItem> items;
  items.reserve(8);
  // Only offered once a dictionary is actually installed -- otherwise the row
  // would open straight into DICTIONARY_NONE_SELECTED every time. Pinned
  // first (user request) with the same label as the Settings/Apps entry
  // (STR_DICTIONARY, not STR_LOOKUP_WORD) so it reads as one feature name
  // whether reached from the reader or from Settings.
  if (DICTIONARIES.hasAnyDictionary()) {
    items.push_back({MenuAction::LOOKUP_WORD, StrId::STR_DICTIONARY});
  }
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::FONT_SIZE, StrId::STR_FONT_SIZE_GENERIC});
  if (isArabicBook || !sdFamilies.empty()) {
    // Arabic books always get the row (two built-in families: Naskh, UthmanicHafs);
    // Latin books only when SD families exist, since Noto Serif is the single
    // built-in serif.
    items.push_back({MenuAction::FONT_NAME, StrId::STR_FONT_NAME});
  }
  items.push_back({MenuAction::LINE_SPACING, StrId::STR_LINE_SPACING_GENERIC});
  items.push_back({MenuAction::TEXT_ALIGN, StrId::STR_TEXT_ALIGNMENT});
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  return items;
}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildSettingsItems(
    const bool hasFootnotes) const {
  std::vector<MenuItem> items;
  items.reserve(9);
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  if (DICTIONARIES.hasAnyDictionary()) {
    items.push_back({MenuAction::LOOKUP_HISTORY, StrId::STR_LOOKUP_HISTORY});
  }
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::RESET_BOOK_SETTINGS, StrId::STR_RESET_BOOK_SETTINGS});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  return items;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

int EpubReaderMenuActivity::activeItemCount() const {
  if (view == View::CHAPTERS) {
    return epub ? epub->getTocItemsCount() : 0;
  }
  return static_cast<int>(activeItems().size());
}

std::string EpubReaderMenuActivity::globalLabel(const char* effectiveValueLabel) const {
  return std::string(tr(STR_GLOBAL_SETTING)) + " (" + effectiveValueLabel + ")";
}

std::string EpubReaderMenuActivity::valueLabel(const MenuAction action) const {
  switch (action) {
    case MenuAction::FONT_SIZE: {
      const uint8_t book = isArabicBook ? SETTINGS.bookArabicFontSize : SETTINGS.bookFontSize;
      const uint8_t global = isArabicBook ? SETTINGS.arabicFontSize : SETTINGS.fontSize;
      return book == CrossPointSettings::BOOK_NO_OVERRIDE
                 ? globalLabel(I18N.get(kSizeLabels[std::min<uint8_t>(global, CrossPointSettings::FONT_SIZE_COUNT - 1)]))
                 : I18N.get(kSizeLabels[book]);
    }
    case MenuAction::FONT_NAME: {
      if (isArabicBook) {
        const char* book = SETTINGS.bookSdArabicFontFamilyName;
        const char* global = SETTINGS.sdArabicFontFamilyName;
        if (book[0] == CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0]) {
          return I18N.get(kSelectableArabicFonts[displayIndexForArabicFamily(SETTINGS.effArabicFontFamily())].first);
        }
        if (book[0] != '\0') return book;
        return globalLabel(
            global[0] != '\0'
                ? global
                : I18N.get(kSelectableArabicFonts[displayIndexForArabicFamily(SETTINGS.arabicFontFamily)].first));
      }
      const char* book = SETTINGS.bookSdFontFamilyName;
      const char* global = SETTINGS.sdFontFamilyName;
      if (book[0] == '\0') {
        return globalLabel(
            global[0] != '\0'
                ? global
                : I18N.get(kSelectableLatinFonts[displayIndexForLatinFamily(SETTINGS.fontFamily)].first));
      }
      if (book[0] == CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0]) {
        return I18N.get(kSelectableLatinFonts[displayIndexForLatinFamily(SETTINGS.effFontFamily())].first);
      }
      return book;
    }
    case MenuAction::TEXT_ALIGN:
      return SETTINGS.bookParagraphAlignment == CrossPointSettings::BOOK_NO_OVERRIDE
                 ? globalLabel(I18N.get(kAlignLabels[std::min<uint8_t>(
                       SETTINGS.paragraphAlignment, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1)]))
                 : I18N.get(kAlignLabels[SETTINGS.bookParagraphAlignment]);
    case MenuAction::LINE_SPACING:
      return SETTINGS.bookLineSpacing == CrossPointSettings::BOOK_NO_OVERRIDE
                 ? globalLabel(I18N.get(kSpacingLabels[std::min<uint8_t>(
                       SETTINGS.lineSpacing, CrossPointSettings::LINE_COMPRESSION_COUNT - 1)]))
                 : I18N.get(kSpacingLabels[SETTINGS.bookLineSpacing]);
    case MenuAction::ROTATE_SCREEN:
      return I18N.get(orientationLabels[pendingOrientation]);
    case MenuAction::AUTO_PAGE_TURN:
      return pageTurnLabels[selectedPageTurnOption];
    default:
      return "";
  }
}

void EpubReaderMenuActivity::openSettingEditor(const MenuAction action) {
  // Enum overrides: option 0 = Global (inherit), then the enum values. Pointer
  // capture (not reference) -- the callback runs after this function returns.
  const auto showEnumPopup = [this](const StrId titleId, const StrId* labels, const int count, uint8_t& field,
                                    const uint8_t globalValue) {
    std::vector<std::string> options;
    options.reserve(count + 1);
    options.push_back(globalLabel(I18N.get(labels[std::min<uint8_t>(globalValue, count - 1)])));
    for (int i = 0; i < count; i++) options.push_back(I18N.get(labels[i]));
    const int current = field == CrossPointSettings::BOOK_NO_OVERRIDE ? 0 : field + 1;
    optionPopup.show(titleId, options, current, [this, fieldPtr = &field](const int idx) {
      const uint8_t newValue = idx == 0 ? CrossPointSettings::BOOK_NO_OVERRIDE : static_cast<uint8_t>(idx - 1);
      if (*fieldPtr != newValue) {
        *fieldPtr = newValue;
        bookSettingsChanged = true;
      }
      requestUpdate();
    });
  };

  switch (action) {
    case MenuAction::FONT_SIZE:
      showEnumPopup(StrId::STR_FONT_SIZE_GENERIC, kSizeLabels, CrossPointSettings::FONT_SIZE_COUNT,
                    isArabicBook ? SETTINGS.bookArabicFontSize : SETTINGS.bookFontSize,
                    isArabicBook ? SETTINGS.arabicFontSize : SETTINGS.fontSize);
      break;
    case MenuAction::TEXT_ALIGN:
      showEnumPopup(StrId::STR_TEXT_ALIGNMENT, kAlignLabels, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT,
                    SETTINGS.bookParagraphAlignment, SETTINGS.paragraphAlignment);
      break;
    case MenuAction::LINE_SPACING:
      showEnumPopup(StrId::STR_LINE_SPACING_GENERIC, kSpacingLabels, CrossPointSettings::LINE_COMPRESSION_COUNT,
                    SETTINGS.bookLineSpacing, SETTINGS.lineSpacing);
      break;
    case MenuAction::FONT_NAME: {
      if (isArabicBook) {
        // Option 0 = Global, 1..N = selectable built-in families (Naskh/UthmanicHafs),
        // N+1.. = SD families. Built-in picks set BOTH the forced-builtin marker and
        // the per-book family value.
        const char* global = SETTINGS.sdArabicFontFamilyName;
        std::vector<std::string> options;
        options.reserve(sdFamilies.size() + 1 + kSelectableArabicFontCount);
        options.push_back(globalLabel(
            global[0] != '\0'
                ? global
                : I18N.get(kSelectableArabicFonts[displayIndexForArabicFamily(SETTINGS.arabicFontFamily)].first)));
        for (const auto& [strId, familyValue] : kSelectableArabicFonts) options.push_back(I18N.get(strId));
        options.insert(options.end(), sdFamilies.begin(), sdFamilies.end());

        int current = 0;
        if (SETTINGS.bookSdArabicFontFamilyName[0] == CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0]) {
          current = 1 + displayIndexForArabicFamily(SETTINGS.effArabicFontFamily());
        } else if (SETTINGS.bookSdArabicFontFamilyName[0] != '\0') {
          for (size_t i = 0; i < sdFamilies.size(); i++) {
            if (sdFamilies[i] == SETTINGS.bookSdArabicFontFamilyName) {
              current = static_cast<int>(1 + kSelectableArabicFontCount + i);
              break;
            }
          }
        }

        optionPopup.show(StrId::STR_FONT_NAME, options, current, [this](const int idx) {
          char newName[32] = "";
          uint8_t newFamily = CrossPointSettings::BOOK_NO_OVERRIDE;
          if (idx >= 1 && idx <= kSelectableArabicFontCount) {
            newName[0] = CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0];
            newFamily = kSelectableArabicFonts[idx - 1].second;
          } else if (idx > kSelectableArabicFontCount &&
                     idx - 1 - kSelectableArabicFontCount < static_cast<int>(sdFamilies.size())) {
            setBookFamily(newName, sizeof(newName), sdFamilies[idx - 1 - kSelectableArabicFontCount].c_str());
          }
          if (strncmp(SETTINGS.bookSdArabicFontFamilyName, newName, sizeof(SETTINGS.bookSdArabicFontFamilyName)) != 0 ||
              SETTINGS.bookArabicFontFamily != newFamily) {
            setBookFamily(SETTINGS.bookSdArabicFontFamilyName, sizeof(SETTINGS.bookSdArabicFontFamilyName), newName);
            SETTINGS.bookArabicFontFamily = newFamily;
            bookSettingsChanged = true;
          }
          requestUpdate();
        });
        break;
      }

      // Latin: option 0 = Global, 1..N = selectable built-in families (Bitter/Lexend
      // Deca), N+1.. = SD families. Built-in picks set BOTH the forced-builtin marker
      // and the per-book family value, mirroring the Arabic branch above.
      const char* global = SETTINGS.sdFontFamilyName;
      std::vector<std::string> options;
      options.reserve(sdFamilies.size() + 1 + kSelectableLatinFontCount);
      options.push_back(globalLabel(
          global[0] != '\0' ? global
                            : I18N.get(kSelectableLatinFonts[displayIndexForLatinFamily(SETTINGS.fontFamily)].first)));
      for (const auto& [strId, familyValue] : kSelectableLatinFonts) options.push_back(I18N.get(strId));
      options.insert(options.end(), sdFamilies.begin(), sdFamilies.end());

      int current = 0;
      if (SETTINGS.bookSdFontFamilyName[0] == CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0]) {
        current = 1 + displayIndexForLatinFamily(SETTINGS.effFontFamily());
      } else if (SETTINGS.bookSdFontFamilyName[0] != '\0') {
        for (size_t i = 0; i < sdFamilies.size(); i++) {
          if (sdFamilies[i] == SETTINGS.bookSdFontFamilyName) {
            current = static_cast<int>(1 + kSelectableLatinFontCount + i);
            break;
          }
        }
      }

      optionPopup.show(StrId::STR_FONT_NAME, options, current, [this](const int idx) {
        char newName[32] = "";
        uint8_t newFamily = CrossPointSettings::BOOK_NO_OVERRIDE;
        if (idx >= 1 && idx <= kSelectableLatinFontCount) {
          newName[0] = CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0];
          newFamily = kSelectableLatinFonts[idx - 1].second;
        } else if (idx > kSelectableLatinFontCount &&
                   idx - 1 - kSelectableLatinFontCount < static_cast<int>(sdFamilies.size())) {
          setBookFamily(newName, sizeof(newName), sdFamilies[idx - 1 - kSelectableLatinFontCount].c_str());
        }
        if (strncmp(SETTINGS.bookSdFontFamilyName, newName, sizeof(SETTINGS.bookSdFontFamilyName)) != 0 ||
            SETTINGS.bookFontFamily != newFamily) {
          setBookFamily(SETTINGS.bookSdFontFamilyName, sizeof(SETTINGS.bookSdFontFamilyName), newName);
          SETTINGS.bookFontFamily = newFamily;
          bookSettingsChanged = true;
        }
        requestUpdate();
      });
      break;
    }
    case MenuAction::RESET_BOOK_SETTINGS:
      if (SETTINGS.hasBookOverrides()) {
        SETTINGS.clearBookOverrides();
        bookSettingsChanged = true;
      }
      requestUpdate();
      break;
    default:
      break;
  }
}

void EpubReaderMenuActivity::finishWithAction(const int action, const bool cancelled) {
  ActivityResult result;
  result.isCancelled = cancelled;
  result.data = MenuResult{action, pendingOrientation, selectedPageTurnOption, bookSettingsChanged};
  setResult(std::move(result));
  finish();
}

void EpubReaderMenuActivity::handleListConfirm() {
  if (view == View::CHAPTERS) {
    if (!epub) return;
    const auto tocItem = epub->getTocItem(chapterSelectedIndex);
    if (tocItem.spineIndex == -1) return;  // non-navigable TOC row
    MenuResult menuResult{static_cast<int>(MenuAction::SELECT_CHAPTER), pendingOrientation, selectedPageTurnOption,
                          bookSettingsChanged};
    menuResult.chapterSpineIndex = tocItem.spineIndex;
    menuResult.chapterAnchor = tocItem.anchor;
    ActivityResult result;
    result.isCancelled = false;
    result.data = std::move(menuResult);
    setResult(std::move(result));
    finish();
    return;
  }

  const auto selectedAction = activeItems()[activeIndex()].action;
  switch (selectedAction) {
    case MenuAction::SELECT_CHAPTER:
      // Chapter list is an in-drawer drill-down, not a separate full-screen activity.
      view = View::CHAPTERS;
      chapterSelectedIndex = epub ? std::max(0, epub->getTocIndexForSpineIndex(currentSpineIndex)) : 0;
      requestUpdate();
      return;
    case MenuAction::ROTATE_SCREEN:
      optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                       pendingOrientation, [this](int idx) {
                         pendingOrientation = idx;
                         requestUpdate();
                       });
      requestUpdate();
      return;
    case MenuAction::AUTO_PAGE_TURN:
      optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), pageTurnLabels.data(),
                       static_cast<int>(pageTurnLabels.size()), selectedPageTurnOption, [this](int idx) {
                         selectedPageTurnOption = idx;
                         requestUpdate();
                       });
      requestUpdate();
      return;
    case MenuAction::FONT_SIZE:
    case MenuAction::FONT_NAME:
    case MenuAction::TEXT_ALIGN:
    case MenuAction::LINE_SPACING:
    case MenuAction::RESET_BOOK_SETTINGS:
      openSettingEditor(selectedAction);
      requestUpdate();
      return;
    default:
      finishWithAction(static_cast<int>(selectedAction), /*cancelled=*/false);
      return;
  }
}

void EpubReaderMenuActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // The tab row is a stop in the same up/down flow as the list below it (index
  // -1), mirroring Settings' "category tabs are index 0" pattern -- scrolling
  // up from the first row reaches the tabs and the selector visibly rests on
  // one, and Confirm there cycles tabs the same way Settings' Confirm-on-tab
  // does. Not offered while browsing Chapters -- that's a drill-down from
  // Reading, not a third tab; Back below returns it to Reading instead.
  //
  // Deliberately NOT also bound to Left/Right: those are legitimate list-
  // scroll shortcuts too (MappedInputManager::Button::ScrollNext/Previous map
  // the front Left/Right buttons to the same next/previous-row action as the
  // physical Up/Down side buttons -- "front buttons doubling as shortcuts for
  // the side Up/Down buttons"). An earlier version of this code intercepted
  // Left/Right for tab-switching before they reached buttonNavigator below,
  // which silently broke list scrolling via the front buttons entirely (user
  // report: "no matter which button I press, it never scrolls, it just
  // switches tabs").
  const bool tabsActive = view != View::CHAPTERS;
  const bool onTabRow = tabsActive && activeIndex() == -1;

  const int itemCount = activeItemCount();

  // Handle navigation. When tabs aren't offered (Chapters), the range is the
  // plain [0, itemCount); otherwise it's [-1, itemCount) with -1 = tab row.
  buttonNavigator.onScrollNext([this, itemCount, tabsActive] {
    activeIndex() = tabsActive ? ButtonNavigator::nextIndex(activeIndex() + 1, itemCount + 1) - 1
                               : ButtonNavigator::nextIndex(activeIndex(), itemCount);
    requestUpdate();
  });

  buttonNavigator.onScrollPrevious([this, itemCount, tabsActive] {
    activeIndex() = tabsActive ? ButtonNavigator::previousIndex(activeIndex() + 1, itemCount + 1) - 1
                               : ButtonNavigator::previousIndex(activeIndex(), itemCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (onTabRow) {
      view = view == View::READING ? View::SETTINGS_TAB : View::READING;
      activeIndex() = -1;
      requestUpdate();
      return;
    }
    if (itemCount > 0) handleListConfirm();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (view == View::CHAPTERS) {
      view = View::READING;
      requestUpdate();
      return;
    }
    finishWithAction(-1, /*cancelled=*/true);
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  // Bottom drawer over the live page: no clearScreen -- the reader's page is
  // still in the framebuffer, so its top stays visible above the drawer. The
  // list paginates inside the drawer when the items don't fit (drawList).
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int drawerTop = pageHeight * 3 / 20;  // drawer covers the bottom 85%
  renderer.fillRect(0, drawerTop, pageWidth, pageHeight - drawerTop, false);
  constexpr int handleWidth = 48;
  renderer.fillRoundedRect((pageWidth - handleWidth) / 2, drawerTop + 5, handleWidth, 6, 3, Color::Black);

  // Material-style header: compact one-line black bar, title in white,
  // no battery. Sized off the font line height (not theme headerHeight) so
  // it stays a slim single line on every theme.
  const int headerTop = drawerTop + 14;
  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int headerHeight = titleLineHeight + 12;
  renderer.fillRect(0, headerTop, pageWidth, headerHeight, true);
  {
    const int titleMargin = 16;
    const std::string shownTitle =
        renderer.truncatedText(UI_10_FONT_ID, title.c_str(), pageWidth - titleMargin * 2, EpdFontFamily::BOLD);
    const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, shownTitle.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, (pageWidth - titleWidth) / 2, headerTop + 6, shownTitle.c_str(), /*black=*/false,
                      EpdFontFamily::BOLD);
  }

  // Progress summary on a light-gray dithered band under the header.
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  const int subTop = headerTop + headerHeight;
  const int subLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int subHeight = subLineHeight + 10;
  renderer.fillRectDither(0, subTop, pageWidth, subHeight, Color::LightGray);
  {
    const int progressWidth = renderer.getTextWidth(SMALL_FONT_ID, progressLine.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, (pageWidth - progressWidth) / 2, subTop + 5, progressLine.c_str(), true,
                      EpdFontFamily::BOLD);
  }

  // Two icon tabs (book = Reading, gear = Settings), each centered in its own
  // half of the drawer width -- not clustered together in the middle. Hidden
  // while browsing Chapters -- that's a drill-down from Reading, not a third
  // tab, so it uses the full content area below like the old MORE view did.
  const bool showTabs = view != View::CHAPTERS;
  const bool onTabRow = showTabs && activeIndex() == -1;
  const int tabBarTop = subTop + subHeight + metrics.verticalSpacing;
  // Must match BookIcon/Settings2Icon's actual bitmap size exactly -- drawIcon
  // has no scaling; it reads the bitmap assuming size x size packed rows.
  constexpr int kTabIconSize = 32;
  constexpr int kTabPadding = 10;    // around the icon inside the focused-row pill
  constexpr int kTabUnderlineGap = 4;
  constexpr int kTabUnderlineHeight = 2;
  // Reserve room for the taller of the two states (the focused pill) so the
  // content below never shifts as focus moves on/off the tab row.
  const int tabBarHeight = showTabs ? (kTabIconSize + kTabPadding * 2 + metrics.verticalSpacing) : 0;
  if (showTabs) {
    const int readingX = pageWidth / 4 - kTabIconSize / 2;
    const int settingsX = pageWidth * 3 / 4 - kTabIconSize / 2;
    const int iconY = tabBarTop + (onTabRow ? kTabPadding : 0);
    // Focus resting on the tab row itself (index -1, reached by scrolling up
    // past the first list row) gets a filled pill behind the active icon,
    // mirroring BaseTheme::drawTabBar's row-focused inversion; otherwise the
    // active tab just gets a plain underline.
    if (onTabRow) {
      const int pillX = (view == View::READING ? readingX : settingsX) - kTabPadding;
      renderer.fillRoundedRect(pillX, tabBarTop, kTabIconSize + kTabPadding * 2, kTabIconSize + kTabPadding * 2,
                               kTabPadding, Color::LightGray);
    }
    renderer.drawIcon(BookIcon, readingX, iconY, kTabIconSize);
    renderer.drawIcon(Settings2Icon, settingsX, iconY, kTabIconSize);
    if (!onTabRow) {
      const int underlineY = iconY + kTabIconSize + kTabUnderlineGap;
      const int underlineX = view == View::READING ? readingX : settingsX;
      renderer.fillRect(underlineX - 2, underlineY, kTabIconSize + 4, kTabUnderlineHeight, true);
    }
  }

  const int contentTop = tabBarTop + tabBarHeight;
  // Safe area already excludes the button-hints strip at the bottom.
  const int contentHeight = (screen.y + screen.height) - contentTop - metrics.verticalSpacing;

  const int itemCount = activeItemCount();
  const int index = view == View::SETTINGS_TAB ? settingsSelectedIndex
                                                : (view == View::CHAPTERS ? chapterSelectedIndex : selectedIndex);
  // The chapter list draws Arabic surah titles through the same compressed-font path
  // as the reader page, but -- unlike renderContents() -- never prewarmed them, so
  // every row hit FontDecompressor's slow per-glyph hot-group fallback on every
  // redraw (moving the selection redraws the visible rows each time). Scan the rows
  // once, prewarm, then draw for real -- the same two-pass pattern the reader already
  // uses. Harmless (nearly free) for the non-Arabic Settings-tab branch too.
  auto* fcm = renderer.getFontCacheManager();
  auto renderRows = [&](FontCacheManager::PrewarmScope* scope) {
    if (view == View::CHAPTERS) {
      GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, itemCount, index, [this](int i) {
        auto item = epub->getTocItem(i);
        std::string indent((item.level - 1) * 2, ' ');
        return indent + item.title;
      });
    } else {
      const auto& items = activeItems();
      GUI.drawList(
          renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, itemCount, index,
          [&items](int i) { return std::string(I18N.get(items[i].labelId)); }, nullptr, nullptr,
          [this, &items](int i) { return valueLabel(items[i].action); }, true);
    }
    if (scope) scope->endScanAndPrewarm();
  };
  if (fcm) {
    auto scope = fcm->createPrewarmScope();
    renderRows(&scope);  // scan pass: drawList's internal drawText calls just record text
  }
  renderRows(nullptr);  // real draw pass

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN), /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
