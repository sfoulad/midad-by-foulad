#include "ArabicFontSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "ArabicFontSystem.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

ArabicFontSelectionActivity::ArabicFontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const SdCardFontRegistry* registry)
    : Activity("ArabicFontSelect", renderer, mappedInput), registry_(registry) {}

void ArabicFontSelectionActivity::onEnter() {
  Activity::onEnter();

  familyNames_.clear();
  familyNames_.push_back(tr(STR_NONE_OPT));
  if (registry_) {
    for (const auto& family : registry_->getFamilies()) {
      familyNames_.push_back(family.name);
    }
  }

  selectedIndex_ = 0;
  if (SETTINGS.sdArabicFontFamilyName[0] != '\0') {
    for (size_t i = 1; i < familyNames_.size(); i++) {
      if (familyNames_[i] == SETTINGS.sdArabicFontFamilyName) {
        selectedIndex_ = static_cast<int>(i);
        break;
      }
    }
  }

  requestUpdate();
}

void ArabicFontSelectionActivity::handleSelection() {
  if (selectedIndex_ == 0) {
    SETTINGS.sdArabicFontFamilyName[0] = '\0';
  } else {
    strncpy(SETTINGS.sdArabicFontFamilyName, familyNames_[selectedIndex_].c_str(),
            sizeof(SETTINGS.sdArabicFontFamilyName) - 1);
    SETTINGS.sdArabicFontFamilyName[sizeof(SETTINGS.sdArabicFontFamilyName) - 1] = '\0';
  }
  SETTINGS.saveToFile();
  arabicFontSystem.ensureLoaded(renderer);
  finish();
}

void ArabicFontSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int listSize = static_cast<int>(familyNames_.size());
  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    requestUpdate();
  });
  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });
}

void ArabicFontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ARABIC_FONT));
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    tr(STR_ARABIC_FONT_HINT));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.tabBarHeight;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const char* currentName = SETTINGS.sdArabicFontFamilyName[0] != '\0' ? SETTINGS.sdArabicFontFamilyName : nullptr;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(familyNames_.size()), selectedIndex_,
      [this](int index) { return familyNames_[index]; }, nullptr, nullptr,
      [this, currentName](int index) -> std::string {
        const bool isCurrent =
            (index == 0 && currentName == nullptr) || (currentName && familyNames_[index] == currentName);
        return isCurrent ? tr(STR_SELECTED) : "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
