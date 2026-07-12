#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>

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

}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks,
                                               const bool isArabicBook)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      isArabicBook(isArabicBook),
      sdFamilies(sdFamilyNames(isArabicBook ? arabicFontSystem.registry() : sdFontSystem.registry())),
      title(title),
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
  items.reserve(8);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::FONT_SIZE, StrId::STR_FONT_SIZE_GENERIC});
  if (!sdFamilies.empty()) {
    // With built-ins only there is exactly one family per script -- nothing to pick.
    items.push_back({MenuAction::FONT_NAME, StrId::STR_FONT_NAME});
  }
  items.push_back({MenuAction::TEXT_ALIGN, StrId::STR_TEXT_ALIGNMENT});
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::MORE, StrId::STR_MORE});
  return items;
}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMoreItems(const bool hasFootnotes) const {
  std::vector<MenuItem> items;
  items.reserve(10);
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::LINE_SPACING, StrId::STR_LINE_SPACING_GENERIC});
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
      const char* book = isArabicBook ? SETTINGS.bookSdArabicFontFamilyName : SETTINGS.bookSdFontFamilyName;
      const char* global = isArabicBook ? SETTINGS.sdArabicFontFamilyName : SETTINGS.sdFontFamilyName;
      const char* builtin = I18N.get(isArabicBook ? StrId::STR_NOTO_NASKH_ARABIC : StrId::STR_NOTO_SERIF);
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
      // Option 0 = Global, 1 = built-in family, 2+ = SD families (script-appropriate).
      char* field = isArabicBook ? SETTINGS.bookSdArabicFontFamilyName : SETTINGS.bookSdFontFamilyName;
      const size_t fieldSize =
          isArabicBook ? sizeof(SETTINGS.bookSdArabicFontFamilyName) : sizeof(SETTINGS.bookSdFontFamilyName);
      const char* global = isArabicBook ? SETTINGS.sdArabicFontFamilyName : SETTINGS.sdFontFamilyName;
      const StrId builtinLabel = isArabicBook ? StrId::STR_NOTO_NASKH_ARABIC : StrId::STR_NOTO_SERIF;

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

void EpubReaderMenuActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  int& index = inMore ? moreSelectedIndex : selectedIndex;
  const auto& items = activeItems();

  // Handle navigation
  buttonNavigator.onNext([this, &index, &items] {
    index = ButtonNavigator::nextIndex(index, static_cast<int>(items.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, &index, &items] {
    index = ButtonNavigator::previousIndex(index, static_cast<int>(items.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = items[index].action;
    switch (selectedAction) {
      case MenuAction::MORE:
        inMore = true;
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
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (inMore) {
      inMore = false;
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

  // Material-style header: full-width black bar, book title in white, no battery.
  const int headerTop = drawerTop + 14;
  const int headerHeight = metrics.headerHeight;
  renderer.fillRect(0, headerTop, pageWidth, headerHeight, true);
  {
    const int titleMargin = 16;
    const std::string shownTitle =
        renderer.truncatedText(UI_12_FONT_ID, title.c_str(), pageWidth - titleMargin * 2, EpdFontFamily::BOLD);
    const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, shownTitle.c_str(), EpdFontFamily::BOLD);
    const int titleY = headerTop + (headerHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, (pageWidth - titleWidth) / 2, titleY, shownTitle.c_str(), /*black=*/false,
                      EpdFontFamily::BOLD);
  }

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(renderer, Rect{screen.x, headerTop + headerHeight, screen.width, metrics.tabBarHeight},
                    progressLine.c_str());

  const int contentTop = headerTop + headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  // Safe area already excludes the button-hints strip at the bottom.
  const int contentHeight = (screen.y + screen.height) - contentTop - metrics.verticalSpacing;

  const auto& items = activeItems();
  const int index = inMore ? moreSelectedIndex : selectedIndex;
  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, static_cast<int>(items.size()), index,
      [&items](int i) { return std::string(I18N.get(items[i].labelId)); }, nullptr, nullptr,
      [this, &items](int i) { return valueLabel(items[i].action); }, true);

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
