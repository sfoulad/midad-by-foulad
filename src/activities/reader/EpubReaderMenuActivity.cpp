#include "EpubReaderMenuActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "ArabicFontSystem.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
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
  mainItems = buildMainItems(hasBookmarks);
  moreItems = buildMoreItems(hasFootnotes);
}

// Most-used first: chapters, bookmarking, then the per-book reading settings,
// then orientation; everything else is one level down behind "More".
std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMainItems(const bool hasBookmarks) const {
  std::vector<MenuItem> items;
  items.reserve(9);
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
  items.push_back({MenuAction::MORE, StrId::STR_MORE});
  return items;
}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMoreItems(const bool hasFootnotes) const {
  std::vector<MenuItem> items;
  items.reserve(9);
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
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
      const char* builtin = I18N.get(StrId::STR_NOTO_SERIF);
      if (book[0] == '\0') return globalLabel(global[0] != '\0' ? global : builtin);
      if (book[0] == CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0]) return builtin;
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

      // Latin: option 0 = Global, 1 = built-in Noto Serif, 2+ = SD families.
      char* field = SETTINGS.bookSdFontFamilyName;
      const size_t fieldSize = sizeof(SETTINGS.bookSdFontFamilyName);
      const char* global = SETTINGS.sdFontFamilyName;
      const StrId builtinLabel = StrId::STR_NOTO_SERIF;

      std::vector<std::string> options;
      options.reserve(sdFamilies.size() + 2);
      options.push_back(globalLabel(global[0] != '\0' ? global : I18N.get(builtinLabel)));
      options.push_back(I18N.get(builtinLabel));
      options.insert(options.end(), sdFamilies.begin(), sdFamilies.end());

      int current = 0;
      if (field[0] == CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0]) {
        current = 1;
      } else if (field[0] != '\0') {
        for (size_t i = 0; i < sdFamilies.size(); i++) {
          if (sdFamilies[i] == field) {
            current = static_cast<int>(2 + i);
            break;
          }
        }
      }

      optionPopup.show(StrId::STR_FONT_NAME, options, current, [this, field, fieldSize](const int idx) {
        char newValue[32] = "";
        if (idx == 1) {
          newValue[0] = CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0];
        } else if (idx >= 2 && idx - 2 < static_cast<int>(sdFamilies.size())) {
          setBookFamily(newValue, sizeof(newValue), sdFamilies[idx - 2].c_str());
        }
        if (strncmp(field, newValue, fieldSize) != 0) {
          setBookFamily(field, fieldSize, newValue);
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
      // Chapter list is an in-drawer view, not a separate full-screen activity.
      view = View::CHAPTERS;
      chapterSelectedIndex = epub ? std::max(0, epub->getTocIndexForSpineIndex(currentSpineIndex)) : 0;
      requestUpdate();
      return;
    case MenuAction::MORE:
      view = View::MORE;
      moreSelectedIndex = 0;
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

  const int itemCount = activeItemCount();

  // Handle navigation
  buttonNavigator.onNext([this, itemCount] {
    activeIndex() = ButtonNavigator::nextIndex(activeIndex(), itemCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, itemCount] {
    activeIndex() = ButtonNavigator::previousIndex(activeIndex(), itemCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (itemCount > 0) handleListConfirm();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (view != View::MAIN) {
      view = View::MAIN;
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

  const int contentTop = subTop + subHeight + metrics.verticalSpacing;
  // Safe area already excludes the button-hints strip at the bottom.
  const int contentHeight = (screen.y + screen.height) - contentTop - metrics.verticalSpacing;

  const int itemCount = activeItemCount();
  const int index = view == View::MORE ? moreSelectedIndex : (view == View::CHAPTERS ? chapterSelectedIndex : selectedIndex);
  // The chapter list draws Arabic surah titles through the same compressed-font path
  // as the reader page, but -- unlike renderContents() -- never prewarmed them, so
  // every row hit FontDecompressor's slow per-glyph hot-group fallback on every
  // redraw (moving the selection redraws the visible rows each time). Scan the rows
  // once, prewarm, then draw for real -- the same two-pass pattern the reader already
  // uses. Harmless (nearly free) for the non-Arabic View::MORE/settings branch too.
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
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
