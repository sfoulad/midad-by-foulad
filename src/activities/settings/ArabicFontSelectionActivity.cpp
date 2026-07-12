#include "ArabicFontSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ArabicFontSystem.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* ELLIPSIS_UTF8 = "\xe2\x80\xa6";

// StrIds for the built-in Arabic fonts, in CrossPointSettings::ARABIC_FONT_FAMILY order.
constexpr StrId kBuiltinArabicFontNames[CrossPointSettings::BUILTIN_ARABIC_FONT_COUNT] = {
    StrId::STR_NOTO_NASKH_ARABIC,
    StrId::STR_AMIRI,
};

int findCurrentArabicFontIndex(const SdCardFontRegistry* registry, const char* sdArabicFontFamilyName,
                               uint8_t arabicFontFamily) {
  if (sdArabicFontFamilyName[0] != '\0' && registry) {
    const auto& families = registry->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == sdArabicFontFamilyName) {
        return CrossPointSettings::BUILTIN_ARABIC_FONT_COUNT + i;
      }
    }
  }
  return arabicFontFamily < CrossPointSettings::BUILTIN_ARABIC_FONT_COUNT ? arabicFontFamily : 0;
}
}  // namespace

ArabicFontSelectionActivity::ArabicFontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const SdCardFontRegistry* registry)
    : Activity("ArabicFontSelect", renderer, mappedInput), registry_(registry) {}

void ArabicFontSelectionActivity::onEnter() {
  Activity::onEnter();

  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  bottomReserved = metrics_.buttonHintsHeight + metrics_.verticalSpacing;
  usableHeight = renderer.getScreenHeight() - afterHeader - bottomReserved;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;

  originalArabicFontFamily_ = SETTINGS.arabicFontFamily;
  strncpy(originalSdArabicFontFamilyName_, SETTINGS.sdArabicFontFamilyName,
          sizeof(originalSdArabicFontFamilyName_) - 1);
  originalSdArabicFontFamilyName_[sizeof(originalSdArabicFontFamilyName_) - 1] = '\0';

  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_ARABIC_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));

  for (uint8_t i = 0; i < CrossPointSettings::BUILTIN_ARABIC_FONT_COUNT; i++) {
    fonts_.push_back({I18N.get(kBuiltinArabicFontNames[i]), true, i});
  }

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back(
          {families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_ARABIC_FONT_COUNT + i)});
    }
  }

  selectedIndex_ = findCurrentArabicFontIndex(registry_, SETTINGS.sdArabicFontFamilyName, SETTINGS.arabicFontFamily);
  previewFontIndex_ = selectedIndex_;

  requestUpdate();
}

void ArabicFontSelectionActivity::onExit() { Activity::onExit(); }

void ArabicFontSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.arabicFontFamily = originalArabicFontFamily_;
    strncpy(SETTINGS.sdArabicFontFamilyName, originalSdArabicFontFamilyName_,
            sizeof(SETTINGS.sdArabicFontFamilyName) - 1);
    SETTINGS.sdArabicFontFamilyName[sizeof(SETTINGS.sdArabicFontFamilyName) - 1] = '\0';
    arabicFontSystem.ensureLoaded(renderer);
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == previewFontIndex_) {
      handleSelection();
    } else {
      previewFontIndex_ = selectedIndex_;
      const auto& font = fonts_[selectedIndex_];
      if (font.isBuiltin) {
        SETTINGS.arabicFontFamily = font.settingIndex;
        SETTINGS.sdArabicFontFamilyName[0] = '\0';
      } else if (registry_) {
        const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_ARABIC_FONT_COUNT;
        const auto& families = registry_->getFamilies();
        if (sdIdx < static_cast<int>(families.size())) {
          strncpy(SETTINGS.sdArabicFontFamilyName, families[sdIdx].name.c_str(),
                  sizeof(SETTINGS.sdArabicFontFamilyName) - 1);
          SETTINGS.sdArabicFontFamilyName[sizeof(SETTINGS.sdArabicFontFamilyName) - 1] = '\0';
        }
      }
      arabicFontSystem.ensureLoaded(renderer);
      requestUpdate();
    }
    return;
  }

  const int listSize = static_cast<int>(fonts_.size());
  const int pageItems =
      UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, previewHeight + metrics_.verticalSpacing);

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });
}

void ArabicFontSelectionActivity::handleSelection() {
  const auto& font = fonts_[selectedIndex_];
  if (font.settingIndex < CrossPointSettings::BUILTIN_ARABIC_FONT_COUNT) {
    SETTINGS.arabicFontFamily = font.settingIndex;
    SETTINGS.sdArabicFontFamilyName[0] = '\0';
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_ARABIC_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      strncpy(SETTINGS.sdArabicFontFamilyName, families[sdIdx].name.c_str(),
              sizeof(SETTINGS.sdArabicFontFamilyName) - 1);
      SETTINGS.sdArabicFontFamilyName[sizeof(SETTINGS.sdArabicFontFamilyName) - 1] = '\0';
    }
  }
  arabicFontSystem.ensureLoaded(renderer);
  SETTINGS.saveToFile();
  finish();
}

void ArabicFontSelectionActivity::renderPreviewPane(int top, int height, int fontId, const char* fontName) const {
  const int left = metrics_.previewPadding;
  const int width = renderer.getScreenWidth() - (metrics_.previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelFontId = UI_10_FONT_ID;
  const int labelH = renderer.getTextHeight(labelFontId);
  const int labelGap = 4;
  const int labelReserved = labelH + labelGap + metrics_.previewPadding;

  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s\"", tr(STR_PREVIEW), fontName ? fontName : "");
  const int labelY = top + height - metrics_.previewPadding - labelH;
  renderer.drawText(labelFontId, left, labelY, labelBuf);

  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int innerHeight = height - metrics_.previewPadding - labelReserved;
  const int maxLines = std::max(1, innerHeight / (lineH + 2));

  const char* previewText = I18N.get(StrId::STR_ARABIC_FONT_PREVIEW_TEXT);
  if (auto* fcm = renderer.getFontCacheManager()) {
    char prewarmBuf[256];
    snprintf(prewarmBuf, sizeof(prewarmBuf), "%s %s", previewText, ELLIPSIS_UTF8);
    fcm->prewarmCache(fontId, prewarmBuf, 0x01);
  }

  const auto lines = renderer.wrappedText(fontId, previewText, width, maxLines);

  int y = top + metrics_.previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (const auto& line : lines) {
    if (y + lineH > textBottomLimit) break;
    renderer.drawText(fontId, left, y, line.c_str());
    y += lineH + 2;
  }
}

void ArabicFontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics_.topPadding, pageWidth, metrics_.headerHeight}, tr(STR_ARABIC_FONT));

  const int previewTop = afterHeader;
  const int listTop = previewTop + previewHeight + metrics_.verticalSpacing;
  const int listHeight = usableHeight - previewHeight - metrics_.verticalSpacing;

  // Built-in entries resolve their reading font id directly (no need for the current
  // selection to already be applied). SD entries only get an actual font id once
  // ensureLoaded() has loaded them (done when the preview was applied in loop()), so
  // ask the renderer what "the" Arabic font currently resolves to -- valid here
  // because clearArabicFontIdMappings() makes every fontId resolve to the same
  // loaded SD font while an SD override is active.
  const bool previewIsBuiltin = previewFontIndex_ >= 0 && previewFontIndex_ < static_cast<int>(fonts_.size()) &&
                                fonts_[previewFontIndex_].isBuiltin;
  const int previewFontId = previewIsBuiltin ? ArabicFontSystem::resolveBuiltinReadingFontId(
                                                   fonts_[previewFontIndex_].settingIndex, SETTINGS.arabicFontSize)
                                             : renderer.getResolvedArabicFontId(NOTOSANSARABIC_12_FONT_ID);
  const char* previewFontName = (previewFontIndex_ >= 0 && previewFontIndex_ < static_cast<int>(fonts_.size()))
                                    ? fonts_[previewFontIndex_].name.c_str()
                                    : nullptr;
  renderPreviewPane(previewTop, previewHeight, previewFontId, previewFontName);

  renderer.drawLine(0, listTop - metrics_.verticalSpacing / 2, pageWidth, listTop - metrics_.verticalSpacing / 2);

  const int currentFontIndex =
      findCurrentArabicFontIndex(registry_, originalSdArabicFontFamilyName_, originalArabicFontFamily_);
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(fonts_.size()), selectedIndex_,
      [this](int index) { return fonts_[index].name; }, nullptr, nullptr,
      [this, currentFontIndex](int index) -> std::string {
        if (index == previewFontIndex_ && index != currentFontIndex) return tr(STR_PREVIEW);
        if (index == currentFontIndex) return tr(STR_SELECTED);
        return "";
      },
      true);

  const bool onPreviewed = selectedIndex_ == previewFontIndex_;
  const char* confirmLabel = onPreviewed ? tr(STR_SELECT) : tr(STR_PREVIEW);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
