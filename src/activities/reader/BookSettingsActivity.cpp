#include "BookSettingsActivity.h"

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

// Copy a picked family name into a per-book override field (fixed 32-byte buffer).
void setBookFamily(char* field, const size_t fieldSize, const std::string& name) {
  strncpy(field, name.c_str(), fieldSize - 1);
  field[fieldSize - 1] = '\0';
}

}  // namespace

BookSettingsActivity::BookSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BookSettings", renderer, mappedInput) {}

void BookSettingsActivity::onEnter() {
  Activity::onEnter();

  latinSdFamilies = sdFamilyNames(sdFontSystem.registry());
  arabicSdFamilies = sdFamilyNames(arabicFontSystem.registry());

  rows.clear();
  rows.reserve(7);
  // Font-family rows only when SD families exist -- with built-ins only there
  // is exactly one family per script, so a per-book family choice is a no-op.
  if (!latinSdFamilies.empty()) rows.push_back({Row::ENGLISH_FONT, StrId::STR_FONT_FAMILY});
  rows.push_back({Row::ENGLISH_FONT_SIZE, StrId::STR_FONT_SIZE});
  if (!arabicSdFamilies.empty()) rows.push_back({Row::ARABIC_FONT, StrId::STR_ARABIC_FONT});
  rows.push_back({Row::ARABIC_FONT_SIZE, StrId::STR_ARABIC_FONT_SIZE});
  rows.push_back({Row::LINE_SPACING, StrId::STR_LINE_SPACING});
  rows.push_back({Row::ALIGNMENT, StrId::STR_PARA_ALIGNMENT});
  rows.push_back({Row::RESET, StrId::STR_RESET_BOOK_SETTINGS});

  requestUpdate();
}

std::string BookSettingsActivity::globalLabel(const char* effectiveValueLabel) const {
  return std::string(tr(STR_GLOBAL_SETTING)) + " (" + effectiveValueLabel + ")";
}

std::string BookSettingsActivity::valueLabel(const Row row) const {
  switch (row) {
    case Row::ENGLISH_FONT: {
      if (SETTINGS.bookSdFontFamilyName[0] == '\0') {
        return globalLabel(SETTINGS.sdFontFamilyName[0] != '\0' ? SETTINGS.sdFontFamilyName : tr(STR_NOTO_SERIF));
      }
      if (SETTINGS.bookSdFontFamilyName[0] == CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0]) {
        return tr(STR_NOTO_SERIF);
      }
      return SETTINGS.bookSdFontFamilyName;
    }
    case Row::ARABIC_FONT: {
      if (SETTINGS.bookSdArabicFontFamilyName[0] == '\0') {
        return globalLabel(SETTINGS.sdArabicFontFamilyName[0] != '\0' ? SETTINGS.sdArabicFontFamilyName
                                                                      : tr(STR_NOTO_NASKH_ARABIC));
      }
      if (SETTINGS.bookSdArabicFontFamilyName[0] == CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0]) {
        return tr(STR_NOTO_NASKH_ARABIC);
      }
      return SETTINGS.bookSdArabicFontFamilyName;
    }
    case Row::ENGLISH_FONT_SIZE:
      return SETTINGS.bookFontSize == CrossPointSettings::BOOK_NO_OVERRIDE
                 ? globalLabel(I18N.get(kSizeLabels[std::min<uint8_t>(
                       SETTINGS.fontSize, CrossPointSettings::FONT_SIZE_COUNT - 1)]))
                 : I18N.get(kSizeLabels[SETTINGS.bookFontSize]);
    case Row::ARABIC_FONT_SIZE:
      return SETTINGS.bookArabicFontSize == CrossPointSettings::BOOK_NO_OVERRIDE
                 ? globalLabel(I18N.get(kSizeLabels[std::min<uint8_t>(
                       SETTINGS.arabicFontSize, CrossPointSettings::FONT_SIZE_COUNT - 1)]))
                 : I18N.get(kSizeLabels[SETTINGS.bookArabicFontSize]);
    case Row::LINE_SPACING:
      return SETTINGS.bookLineSpacing == CrossPointSettings::BOOK_NO_OVERRIDE
                 ? globalLabel(I18N.get(kSpacingLabels[std::min<uint8_t>(
                       SETTINGS.lineSpacing, CrossPointSettings::LINE_COMPRESSION_COUNT - 1)]))
                 : I18N.get(kSpacingLabels[SETTINGS.bookLineSpacing]);
    case Row::ALIGNMENT:
      return SETTINGS.bookParagraphAlignment == CrossPointSettings::BOOK_NO_OVERRIDE
                 ? globalLabel(I18N.get(kAlignLabels[std::min<uint8_t>(
                       SETTINGS.paragraphAlignment, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1)]))
                 : I18N.get(kAlignLabels[SETTINGS.bookParagraphAlignment]);
    case Row::RESET:
      return "";
  }
  return "";
}

void BookSettingsActivity::openEditorForRow(const Row row) {
  // Simple enum overrides: option 0 = Global (inherit), then the enum values.
  const auto showEnumPopup = [this](const StrId titleId, const StrId* labels, const int count, uint8_t& field,
                                    const uint8_t globalValue) {
    std::vector<std::string> options;
    options.reserve(count + 1);
    options.push_back(globalLabel(I18N.get(labels[std::min<uint8_t>(globalValue, count - 1)])));
    for (int i = 0; i < count; i++) options.push_back(I18N.get(labels[i]));
    const int current = field == CrossPointSettings::BOOK_NO_OVERRIDE ? 0 : field + 1;
    // Capture a pointer, not the reference parameter: the callback runs after
    // this function has returned (the referent is a global SETTINGS field).
    optionPopup.show(titleId, options, current, [this, fieldPtr = &field](const int idx) {
      const uint8_t newValue = idx == 0 ? CrossPointSettings::BOOK_NO_OVERRIDE : static_cast<uint8_t>(idx - 1);
      if (*fieldPtr != newValue) {
        *fieldPtr = newValue;
        changed = true;
      }
      requestUpdate();
    });
  };

  // Font-family overrides: option 0 = Global, 1 = built-in family, 2+ = SD families.
  const auto showFamilyPopup = [this](const StrId titleId, const StrId builtinLabel, const char* globalFamily,
                                      const std::vector<std::string>& sdFamilies, char* field,
                                      const size_t fieldSize) {
    std::vector<std::string> options;
    options.reserve(sdFamilies.size() + 2);
    options.push_back(globalLabel(globalFamily[0] != '\0' ? globalFamily : I18N.get(builtinLabel)));
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

    optionPopup.show(titleId, options, current, [this, sdFamilies, field, fieldSize](const int idx) {
      char newValue[32] = "";
      if (idx == 1) {
        newValue[0] = CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0];
      } else if (idx >= 2 && idx - 2 < static_cast<int>(sdFamilies.size())) {
        setBookFamily(newValue, sizeof(newValue), sdFamilies[idx - 2]);
      }
      if (strncmp(field, newValue, fieldSize) != 0) {
        setBookFamily(field, fieldSize, newValue);
        changed = true;
      }
      requestUpdate();
    });
  };

  switch (row) {
    case Row::ENGLISH_FONT:
      showFamilyPopup(StrId::STR_FONT_FAMILY, StrId::STR_NOTO_SERIF, SETTINGS.sdFontFamilyName, latinSdFamilies,
                      SETTINGS.bookSdFontFamilyName, sizeof(SETTINGS.bookSdFontFamilyName));
      break;
    case Row::ARABIC_FONT:
      showFamilyPopup(StrId::STR_ARABIC_FONT, StrId::STR_NOTO_NASKH_ARABIC, SETTINGS.sdArabicFontFamilyName,
                      arabicSdFamilies, SETTINGS.bookSdArabicFontFamilyName,
                      sizeof(SETTINGS.bookSdArabicFontFamilyName));
      break;
    case Row::ENGLISH_FONT_SIZE:
      showEnumPopup(StrId::STR_FONT_SIZE, kSizeLabels, CrossPointSettings::FONT_SIZE_COUNT, SETTINGS.bookFontSize,
                    SETTINGS.fontSize);
      break;
    case Row::ARABIC_FONT_SIZE:
      showEnumPopup(StrId::STR_ARABIC_FONT_SIZE, kSizeLabels, CrossPointSettings::FONT_SIZE_COUNT,
                    SETTINGS.bookArabicFontSize, SETTINGS.arabicFontSize);
      break;
    case Row::LINE_SPACING:
      showEnumPopup(StrId::STR_LINE_SPACING, kSpacingLabels, CrossPointSettings::LINE_COMPRESSION_COUNT,
                    SETTINGS.bookLineSpacing, SETTINGS.lineSpacing);
      break;
    case Row::ALIGNMENT:
      showEnumPopup(StrId::STR_PARA_ALIGNMENT, kAlignLabels, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT,
                    SETTINGS.bookParagraphAlignment, SETTINGS.paragraphAlignment);
      break;
    case Row::RESET:
      if (SETTINGS.hasBookOverrides()) {
        SETTINGS.clearBookOverrides();
        changed = true;
      }
      break;
  }
}

void BookSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(rows.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(rows.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openEditorForRow(rows[selectedIndex].row);
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    setResult(BookSettingsResult{changed});
    finish();
  }
}

void BookSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  const auto metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // Bottom drawer over the live page: no clearScreen, so the framebuffer keeps
  // the book page visible in the strip above the drawer.
  const int drawerTop = pageHeight / 5;
  renderer.fillRect(0, drawerTop, pageWidth, pageHeight - drawerTop, false);
  renderer.fillRect(0, drawerTop, pageWidth, 3, true);
  // Grab handle, drawer-style visual cue.
  const int handleWidth = 48;
  renderer.fillRoundedRect((pageWidth - handleWidth) / 2, drawerTop + 8, handleWidth, 6, 3, Color::Black);

  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int headerTop = drawerTop + 16;
  GUI.drawHeader(renderer, Rect{screen.x, headerTop, screen.width, metrics.headerHeight}, tr(STR_BOOK_SETTINGS));

  // Safe area already excludes the button-hints strip at the bottom.
  const int contentTop = headerTop + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = (screen.y + screen.height) - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, static_cast<int>(rows.size()), selectedIndex,
      [this](int index) { return std::string(I18N.get(rows[index].labelId)); }, nullptr, nullptr,
      [this](int index) { return valueLabel(rows[index].row); }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
