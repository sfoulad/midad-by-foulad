#include "GymActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdio>

#include "GymAssetSyncActivity.h"
#include "GymCatalog.h"
#include "GymCatalogSyncActivity.h"
#include "GymDayDetailActivity.h"
#include "GymPlanStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

GymActivity::GymActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Gym", renderer, mappedInput) {}

void GymActivity::onEnter() {
  Activity::onEnter();
  refreshCatalogStatus();
  selectedIndex_ = GYM_PLAN.getCurrentDayIndex();
  requestUpdate();
}

void GymActivity::refreshCatalogStatus() { hasCatalog_ = Storage.exists(GymCatalog::CATALOG_PATH); }

void GymActivity::launchCatalogSync() {
  startActivityForResult(std::make_unique<GymCatalogSyncActivity>(renderer, mappedInput), [this](const ActivityResult&) {
    refreshCatalogStatus();
    requestUpdate();
  });
}

void GymActivity::openSelectedDay() {
  startActivityForResult(
      std::make_unique<GymDayDetailActivity>(renderer, mappedInput, static_cast<size_t>(selectedIndex_)),
      [this](const ActivityResult&) { requestUpdate(); });
}

void GymActivity::launchAssetSync() {
  startActivityForResult(std::make_unique<GymAssetSyncActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void GymActivity::loop() {
  if (longPressConfirmFired_ || longPressBackFired_) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressConfirmFired_ = false;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) {
      longPressBackFired_ = false;
    }
    return;
  }

  // Long-press Back: sync missing exercise assets across all 7 days (a
  // global action, independent of the selected day). Checked before the
  // short-press Back handler below.
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressBackFired_ = true;
    launchAssetSync();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (!hasCatalog_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      launchCatalogSync();
    }
    return;
  }

  // Long-press Confirm: toggle the selected day as a Rest Day (only takes
  // effect when that day has zero exercises -- see GymPlanStore::toggleRestDay).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressConfirmFired_ = true;
    GYM_PLAN.toggleRestDay(static_cast<size_t>(selectedIndex_));
    requestUpdate();
    return;
  }

  const int dayCount = static_cast<int>(GymPlanStore::DAY_COUNT);
  buttonNavigator_.onScrollNextRelease([this, dayCount] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, dayCount);
    requestUpdate();
  });
  buttonNavigator_.onScrollPreviousRelease([this, dayCount] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, dayCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelectedDay();
  }
}

void GymActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GYM));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (!hasCatalog_) {
    const int msgWidth = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_NO_GYM_CATALOG));
    renderer.drawText(UI_10_FONT_ID, (pageWidth - msgWidth) / 2, contentTop + 40, tr(STR_NO_GYM_CATALOG));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SYNC), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const int dayCount = static_cast<int>(GymPlanStore::DAY_COUNT);
    const int currentDay = GYM_PLAN.getCurrentDayIndex();
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, dayCount, selectedIndex_,
        [](int i) {
          char buf[16];
          snprintf(buf, sizeof(buf), tr(STR_GYM_DAY_FORMAT), i + 1);
          return std::string(buf);
        },
        [](int i) {
          const auto& day = GYM_PLAN.getDay(static_cast<size_t>(i));
          if (day.isRestDay) return std::string(tr(STR_REST_DAY));
          char buf[32];
          snprintf(buf, sizeof(buf), tr(STR_EXERCISE_COUNT_FORMAT), static_cast<int>(day.exercises.size()));
          return std::string(buf);
        },
        nullptr,
        [currentDay](int i) { return i == currentDay ? std::string(tr(STR_TODAY)) : std::string(); }, false);

    // The day list rarely fills the whole content area (7 rows at the
    // with-subtitle row height), leaving spare room below it -- use that
    // space to surface the two long-press actions, which otherwise have no
    // visual hint anywhere on this screen.
    const int listBottom = contentTop + dayCount * metrics.listWithSubtitleRowHeight;
    const int hintLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int hintY = listBottom + metrics.verticalSpacing;
    if (hintY + hintLineHeight * 2 <= contentTop + contentHeight) {
      const char* hint1 = tr(STR_GYM_HOLD_CONFIRM_HINT);
      const int hint1Width = renderer.getTextWidth(SMALL_FONT_ID, hint1);
      renderer.drawText(SMALL_FONT_ID, (pageWidth - hint1Width) / 2, hintY, hint1);

      const char* hint2 = tr(STR_GYM_HOLD_BACK_HINT);
      const int hint2Width = renderer.getTextWidth(SMALL_FONT_ID, hint2);
      renderer.drawText(SMALL_FONT_ID, (pageWidth - hint2Width) / 2, hintY + hintLineHeight + 4, hint2);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
