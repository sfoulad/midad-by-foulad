#include "EpubReaderMenuActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "ArabicFontSystem.h"
#include "CrossPointSettings.h"
#include "DictionaryStore.h"
#include "FouladEbooksConfig.h"
#include "MappedInputManager.h"
#include "MidadAppSettings.h"
#include "OpdsServerStore.h"
#include "ReaderFontSizes.h"
#include "ReaderPomodoro.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
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
                                               const int currentSpineIndex, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks,
                                               const bool isArabicBook, const bool canSyncMidad)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      epub(epub),
      currentSpineIndex(currentSpineIndex),
      isArabicBook(isArabicBook),
      canSyncMidad_(canSyncMidad),
      sdFamilies(sdFamilyNames(isArabicBook ? arabicFontSystem.registry() : sdFontSystem.registry())),
      title(epub ? flattenedTitle(epub->getTitle()) : std::string()),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {
  definitionTextSizeLabels.reserve(DictionaryStore::DEF_TEXT_SIZE_COUNT);
  for (uint8_t i = 0; i < DictionaryStore::DEF_TEXT_SIZE_COUNT; ++i) {
    definitionTextSizeLabels.push_back(DictionaryStore::definitionTextSizeLabel(i));
  }

  dictionaryItems = buildDictionaryItems();
  textItems = buildTextItems();
  navigateItems = buildNavigateItems(hasBookmarks, hasFootnotes);
  settingsItems = buildSettingsItems();

  // An installed dictionary is the only thing that varies here. Without one the
  // Dictionary tab has nothing to show and every row on it would open straight into
  // DICTIONARY_NONE_SELECTED, so the tab is dropped rather than shown-and-inert --
  // and the opening view moves to the first tab that does exist.
  if (DICTIONARIES.hasAnyDictionary()) {
    tabOrder.push_back(View::DICTIONARY);
  } else {
    view = View::TEXT;
  }
  tabOrder.push_back(View::TEXT);
  tabOrder.push_back(View::NAVIGATE);
  tabOrder.push_back(View::SETTINGS_TAB);
}

// Four tabs, each a single subject: Dictionary (look things up), Text (how the page
// is set), Navigate (move around the book, bookmarks included) and Settings
// (everything else). The previous two-tab split -- "Reading" and "Settings" -- had
// grown to 13 and 10 rows, with the Settings tab acting as an overflow bin for
// anything that was not a per-book reading setting.
std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildDictionaryItems() const {
  std::vector<MenuItem> items;
  // Only reached at all when a dictionary is installed -- buildTabOrder() drops the
  // whole tab otherwise, so no row here needs its own hasAnyDictionary() guard.
  items.reserve(4);
  // Labelled "Look up a word", not "Dictionary". On the old Reading tab the row was
  // deliberately named for the feature, so it read the same whether reached from the
  // reader or from Settings; inside a tab already called Dictionary the tab supplies
  // that name and the row is free to say what it does.
  items.push_back({MenuAction::LOOKUP_WORD, StrId::STR_LOOKUP_WORD});
  items.push_back({MenuAction::LOOKUP_HISTORY, StrId::STR_LOOKUP_HISTORY});
  // The reason this tab exists: switching dictionaries mid-book used to mean leaving
  // the reader for Settings -> Dictionary and finding your place again. Just
  // "Dictionary" -- the value column carries which one is in use, so the label does
  // not have to.
  items.push_back({MenuAction::ACTIVE_DICTIONARY, StrId::STR_DICTIONARY});
  items.push_back({MenuAction::DEFINITION_TEXT_SIZE, StrId::STR_DEFINITION_TEXT_SIZE});
  return items;
}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildTextItems() const {
  std::vector<MenuItem> items;
  items.reserve(8);
  items.push_back({MenuAction::FONT_SIZE, StrId::STR_FONT_SIZE_GENERIC});
  if (isArabicBook || !sdFamilies.empty()) {
    // Arabic books always get the row (two built-in families: Naskh, UthmanicHafs);
    // Latin books only when SD families exist, since Noto Serif is the single
    // built-in serif.
    items.push_back({MenuAction::FONT_NAME, StrId::STR_FONT_NAME});
  }
  items.push_back({MenuAction::LINE_SPACING, StrId::STR_LINE_SPACING_GENERIC});
  items.push_back({MenuAction::TEXT_ALIGN, StrId::STR_TEXT_ALIGNMENT});
  // Night mode, frontlight and orientation are not typography, but they are the
  // rest of "how the page looks", and splitting them off would leave Text with four
  // rows and push three into a tab about going places.
  items.push_back({MenuAction::NIGHT_MODE, StrId::STR_NIGHT_MODE});
  if (Frontlight.present()) {
    items.push_back({MenuAction::FRONTLIGHT, StrId::STR_FRONTLIGHT});
  }
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  // Last, and on this tab rather than Settings, because clearBookOverrides() resets
  // exactly the rows above it and nothing else.
  items.push_back({MenuAction::RESET_BOOK_SETTINGS, StrId::STR_RESET_BOOK_SETTINGS});
  return items;
}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildNavigateItems(
    const bool hasBookmarks, const bool hasFootnotes) const {
  std::vector<MenuItem> items;
  items.reserve(6);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  // Automated page advancement is still page advancement: it moves you through the
  // book, which is what this tab is for, even though it is set rather than pressed.
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  return items;
}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildSettingsItems() const {
  std::vector<MenuItem> items;
  items.reserve(7);
  // Both syncs together. Midad was pinned to the old Reading tab because it is
  // pressed every session, but with four tabs it no longer has to be first to be
  // one press away, and having the two sync rows in different tabs was the odder
  // arrangement. Shown on the strength of the account alone, not on whether THIS
  // book carries a catalog id: a book that genuinely is in the library -- but was
  // only ever opened from Home, so its id was never recorded -- would otherwise
  // have no row at all, indistinguishable from the feature being broken. It
  // explains itself when pressed; canSyncMidad_ decides which of the two it does.
  {
    const auto& opdsServers = OPDS_STORE.getServers();
    const bool hasMidadAccount = std::any_of(opdsServers.begin(), opdsServers.end(),
                                             [](const OpdsServer& s) { return s.url == FOULAD_EBOOKS_URL; });
    if (hasMidadAccount) {
      items.push_back({MenuAction::MIDAD_SYNC, StrId::STR_SYNC_MIDAD});
    }
  }
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_KOREADER});
  // Gated on the same Settings -> Apps -> Pomodoro toggle that pins the My Books
  // tile, so a device that does not use the feature sees no row at all.
  if (MIDAD_APP_SETTINGS.pomodoroEnabled) {
    items.push_back({MenuAction::POMODORO, StrId::STR_POMODORO});
  }
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  return items;
}

int EpubReaderMenuActivity::tabPosition(const View v) const {
  const auto it = std::find(tabOrder.begin(), tabOrder.end(), v);
  return it == tabOrder.end() ? 0 : static_cast<int>(it - tabOrder.begin());
}

const char* EpubReaderMenuActivity::tabLabel(const View v) const {
  switch (v) {
    case View::TEXT:
      return tr(STR_TAB_TEXT);
    case View::NAVIGATE:
      return tr(STR_TAB_NAVIGATE);
    case View::SETTINGS_TAB:
      return tr(STR_SETTINGS_TITLE);
    default:
      return tr(STR_DICTIONARY);
  }
}

// Forward with wraparound, exactly as SettingsActivity cycles its five categories.
// Left/Right deliberately stay out of this: they are list-scroll shortcuts too, and
// binding them to tabs once broke scrolling with the front buttons entirely.
EpubReaderMenuActivity::View EpubReaderMenuActivity::nextTab() const {
  if (tabOrder.empty()) return view;
  const int next = (tabPosition(view) + 1) % static_cast<int>(tabOrder.size());
  return tabOrder[next];
}

namespace {
// Drawer chrome the render pass always draws, shared with the height calculation so
// the two cannot drift apart.
constexpr int kHandleToHeaderGap = 14;
int headerHeightFor(const GfxRenderer& renderer) { return renderer.getLineHeight(UI_10_FONT_ID) + 12; }
int progressBandHeightFor(const GfxRenderer& renderer) { return renderer.getLineHeight(SMALL_FONT_ID) + 10; }
}  // namespace

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

int EpubReaderMenuActivity::activeItemCount() const {
  if (view == View::CHAPTERS) {
    return epub ? epub->getTocItemsCount() : 0;
  }
  if (view == View::DICTIONARY_LIST) {
    return static_cast<int>(DICTIONARIES.getEntries().size());
  }
  return static_cast<int>(activeItems().size());
}

std::string EpubReaderMenuActivity::globalLabel(const char* effectiveValueLabel) const {
  return std::string(tr(STR_GLOBAL_SETTING)) + " (" + effectiveValueLabel + ")";
}

std::string EpubReaderMenuActivity::valueLabel(const MenuAction action) const {
  switch (action) {
    case MenuAction::FONT_SIZE: {
      if (isArabicBook) {
        const uint8_t book = SETTINGS.bookArabicFontSize;
        const uint8_t global = SETTINGS.arabicFontSize;
        return book == CrossPointSettings::BOOK_NO_OVERRIDE
                   ? globalLabel(
                         I18N.get(kSizeLabels[std::min<uint8_t>(global, CrossPointSettings::FONT_SIZE_COUNT - 1)]))
                   : I18N.get(kSizeLabels[book]);
      }
      // Latin: point-size based (see ReaderFontSizes.h), not the fixed enum --
      // the selectable/snapped sizes depend on whichever family is effectively
      // active for this book (per-book family override, else the global one).
      const std::vector<uint8_t> sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.effSdFontFamilyName());
      char buf[16];
      if (SETTINGS.bookFontSize == CrossPointSettings::BOOK_NO_OVERRIDE) {
        snprintf(buf, sizeof(buf), "%u pt", snapToNearestPointSize(sizes, SETTINGS.fontPointSize));
        return globalLabel(buf);
      }
      snprintf(buf, sizeof(buf), "%u pt", snapToNearestPointSize(sizes, SETTINGS.bookFontSize));
      return buf;
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
            global[0] != '\0' ? global
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
    case MenuAction::NIGHT_MODE:
      return I18N.get(SETTINGS.screenInverted ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
    case MenuAction::FRONTLIGHT:
      return I18N.get(Frontlight.isOn() ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
    case MenuAction::POMODORO: {
      // The row doubles as the session's readout, so the value column says what pressing
      // it will do AND where the session is: "Start Session" when idle, the live
      // countdown while running, the phase-done message when it needs acknowledging.
      const auto& pomodoro = READER_POMODORO;
      if (!pomodoro.isActive()) return I18N.get(StrId::STR_POMODORO_START_SESSION);
      if (pomodoro.isFinished()) {
        return I18N.get(pomodoro.currentPhase() == ReaderPomodoro::Phase::Focus ? StrId::STR_POMODORO_FOCUS_DONE
                                                                                : StrId::STR_POMODORO_BREAK_DONE);
      }
      return formatPomodoroRemaining(pomodoro.remainingMs());
    }
    case MenuAction::ACTIVE_DICTIONARY: {
      // The row is the readout as well as the way in: which dictionary a lookup will
      // use is the single thing someone opens this tab to check.
      const auto& entries = DICTIONARIES.getEntries();
      const int active = DICTIONARIES.getActiveIndex();
      if (active < 0 || active >= static_cast<int>(entries.size())) return I18N.get(StrId::STR_NO_DICTIONARIES);
      return entries[active].name;
    }
    case MenuAction::DEFINITION_TEXT_SIZE:
      return DictionaryStore::definitionTextSizeLabel(DICTIONARIES.getDefinitionTextSize());
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
      if (isArabicBook) {
        showEnumPopup(StrId::STR_FONT_SIZE_GENERIC, kSizeLabels, CrossPointSettings::FONT_SIZE_COUNT,
                      SETTINGS.bookArabicFontSize, SETTINGS.arabicFontSize);
        break;
      }
      {
        // Latin: the option list is a dynamic set of point sizes (built-in fixed
        // set, or an SD family's actually-installed sizes -- see
        // ReaderFontSizes.h), not showEnumPopup's fixed StrId-label shape, so
        // this popup is built directly rather than reusing that helper.
        const std::vector<uint8_t> sizes =
            readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.effSdFontFamilyName());
        std::vector<std::string> options;
        options.reserve(sizes.size() + 1);
        char buf[16];
        snprintf(buf, sizeof(buf), "%u pt", snapToNearestPointSize(sizes, SETTINGS.fontPointSize));
        options.push_back(globalLabel(buf));
        for (const uint8_t pt : sizes) options.push_back(std::to_string(pt) + " pt");

        int current = 0;
        if (SETTINGS.bookFontSize != CrossPointSettings::BOOK_NO_OVERRIDE) {
          const uint8_t snapped = snapToNearestPointSize(sizes, SETTINGS.bookFontSize);
          for (size_t i = 0; i < sizes.size(); i++) {
            if (sizes[i] == snapped) {
              current = static_cast<int>(1 + i);
              break;
            }
          }
        }

        optionPopup.show(StrId::STR_FONT_SIZE_GENERIC, options, current, [this, sizes](const int idx) {
          const uint8_t newValue = (idx == 0 || sizes.empty()) ? CrossPointSettings::BOOK_NO_OVERRIDE
                                                               : sizes[std::min<size_t>(idx - 1, sizes.size() - 1)];
          if (SETTINGS.bookFontSize != newValue) {
            SETTINGS.bookFontSize = newValue;
            bookSettingsChanged = true;
          }
          requestUpdate();
        });
      }
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

  if (view == View::DICTIONARY_LIST) {
    // Picking a dictionary is the whole point of the tab: set it active and go
    // straight back, without leaving the drawer or the book.
    //
    // setActiveIndex() refuses a folder with missing files or a 64-bit-offset .ifo,
    // and ignoring that refusal would look exactly like a successful switch -- back
    // to the tab, with the row still naming the old dictionary. Say so instead, in
    // the same words Apps -> Dictionary uses for the same two cases.
    if (!DICTIONARIES.setActiveIndex(dictionarySelectedIndex)) {
      GUI.drawPopup(renderer, tr(STR_DICTIONARY_MISSING_FILES));
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      delay(1100);
      requestUpdate();
      return;
    }
    view = dictionaryListReturnTo;
    requestUpdate();
    return;
  }

  const auto selectedAction = activeItems()[activeIndex()].action;
  switch (selectedAction) {
    case MenuAction::SELECT_CHAPTER:
      // Chapter list is an in-drawer drill-down, not a separate full-screen activity.
      chapterReturnTo = view;
      view = View::CHAPTERS;
      chapterSelectedIndex = epub ? std::max(0, epub->getTocIndexForSpineIndex(currentSpineIndex)) : 0;
      requestUpdate();
      return;
    case MenuAction::ACTIVE_DICTIONARY: {
      DICTIONARIES.ensureScanned();
      dictionaryListReturnTo = view;
      view = View::DICTIONARY_LIST;
      // Open on the dictionary in use, so Confirm without moving is a no-op rather
      // than a silent switch to whatever happens to sit at index 0.
      dictionarySelectedIndex = std::max(0, DICTIONARIES.getActiveIndex());
      requestUpdate();
      return;
    }
    case MenuAction::DEFINITION_TEXT_SIZE:
      optionPopup.show(I18N.get(StrId::STR_DEFINITION_TEXT_SIZE), definitionTextSizeLabels.data(),
                       static_cast<int>(definitionTextSizeLabels.size()), DICTIONARIES.getDefinitionTextSize(),
                       [this](int idx) {
                         DICTIONARIES.setDefinitionTextSize(static_cast<uint8_t>(idx));
                         requestUpdate();
                       });
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
    case MenuAction::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      requestUpdate();
      return;
    case MenuAction::FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      requestUpdate();
      return;
    }
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
  const bool tabsActive = view != View::CHAPTERS && view != View::DICTIONARY_LIST;
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
      view = nextTab();
      activeIndex() = -1;
      requestUpdate();
      return;
    }
    if (itemCount > 0) handleListConfirm();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Back out of a drill-down returns to the tab it was opened from, not to the
    // reader -- one Back should never cost you both the list and the drawer.
    if (view == View::CHAPTERS) {
      view = chapterReturnTo;
      requestUpdate();
      return;
    }
    if (view == View::DICTIONARY_LIST) {
      view = dictionaryListReturnTo;
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
  // The drawer is only as tall as its tallest tab needs, rather than a fixed 85% of
  // the screen. At four tabs the largest list is 8 rows, and the old fixed height
  // left room for 11 -- three rows of empty drawer over a page the reader would
  // rather see. Derived instead of tuned so it holds on both panels and however the
  // conditional rows land: everything below is the chrome this view always draws,
  // plus the rows, plus the hint strip the safe area excludes.
  const int maxRows = std::max({dictionaryItems.size(), textItems.size(), navigateItems.size(), settingsItems.size()});
  const int neededHeight = kHandleToHeaderGap + headerHeightFor(renderer) + progressBandHeightFor(renderer) +
                           metrics.tabBarHeight + maxRows * metrics.listRowHeight + metrics.verticalSpacing +
                           metrics.buttonHintsHeight;
  // Never taller than the old fixed height: a long drill-down list paginates, which
  // it already did, and growing the drawer past 85% would start hiding the page the
  // menu is meant to sit over.
  const int drawerTop = std::max(pageHeight * 3 / 20, pageHeight - neededHeight);
  renderer.fillRect(0, drawerTop, pageWidth, pageHeight - drawerTop, false);
  constexpr int handleWidth = 48;
  renderer.fillRoundedRect((pageWidth - handleWidth) / 2, drawerTop + 5, handleWidth, 6, 3, Color::Black);

  // Material-style header: compact one-line black bar, title in white,
  // no battery. Sized off the font line height (not theme headerHeight) so
  // it stays a slim single line on every theme.
  const int headerTop = drawerTop + kHandleToHeaderGap;
  const int headerHeight = headerHeightFor(renderer);
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
  const int subHeight = progressBandHeightFor(renderer);
  renderer.fillRectDither(0, subTop, pageWidth, subHeight, Color::LightGray);
  {
    const int progressWidth = renderer.getTextWidth(SMALL_FONT_ID, progressLine.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, (pageWidth - progressWidth) / 2, subTop + 5, progressLine.c_str(), true,
                      EpdFontFamily::BOLD);
  }

  // Text tabs through the shared theme helper -- the same one Settings uses for its
  // five categories -- rather than icons laid out at fixed fractions of the width.
  // The strip is a vector, so it copes with the Dictionary tab being absent without
  // any of this arithmetic knowing how many tabs there are. Hidden inside a
  // drill-down, which uses the full content area below.
  const bool showTabs = view != View::CHAPTERS && view != View::DICTIONARY_LIST;
  const bool onTabRow = showTabs && activeIndex() == -1;
  // No gap between the progress band and the tab strip: the band's own fill already
  // separates them, and the spacer only read as a dead stripe.
  const int tabBarTop = subTop + subHeight;
  const int tabBarHeight = showTabs ? metrics.tabBarHeight : 0;
  if (showTabs) {
    std::vector<TabInfo> tabs;
    tabs.reserve(tabOrder.size());
    for (const View v : tabOrder) {
      tabs.push_back({tabLabel(v), v == view});
    }
    // `selected` = focus is resting on the strip itself (index -1), which inverts the
    // active tab rather than merely underlining it -- the theme's own convention.
    GUI.drawTabBar(renderer, Rect{0, tabBarTop, pageWidth, tabBarHeight}, tabs, onTabRow);
  }

  const int contentTop = tabBarTop + tabBarHeight;
  // Safe area already excludes the button-hints strip at the bottom.
  const int contentHeight = (screen.y + screen.height) - contentTop - metrics.verticalSpacing;

  const int itemCount = activeItemCount();
  // activeIndex() is the one place that knows which counter each view uses; reading
  // it through a const_cast beats keeping a second switch here that can drift.
  const int index = const_cast<EpubReaderMenuActivity*>(this)->activeIndex();
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
    } else if (view == View::DICTIONARY_LIST) {
      const auto& entries = DICTIONARIES.getEntries();
      const int activeDictionary = DICTIONARIES.getActiveIndex();
      GUI.drawList(
          renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, itemCount, index,
          [&entries](int i) { return entries[i].name; }, nullptr, nullptr,
          // The one in use is marked in the value column rather than by reordering the
          // list, so a dictionary keeps its position as you switch between them.
          [activeDictionary](int i) {
            return i == activeDictionary ? std::string(tr(STR_DICTIONARY_ACTIVE)) : std::string();
          },
          true);
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

  // Footer / Hints. While focus is on the tab row, Confirm cycles forward, so the
  // hint previews the name of the tab it will land on instead of the generic
  // "Select" -- same convention as Settings' own tab row (SettingsActivity.cpp's
  // confirmLabel). With four tabs that preview earns its keep: cycling is the only
  // way across, so knowing the next stop is what makes it navigable.
  const char* confirmLabel = onTabRow ? tabLabel(nextTab()) : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN),
                                            /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
