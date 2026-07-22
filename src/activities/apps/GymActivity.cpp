#include "GymActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "GymAssetSyncActivity.h"
#include "GymCatalog.h"
#include "GymCatalogSyncActivity.h"
#include "GymDayDetailActivity.h"
#include "GymPlanStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Body-part strings come straight from the source dataset (English only,
// lowercase) -- capitalized only for display, same convention as
// GymExerciseBrowserActivity's own local capitalize().
std::string capitalize(const std::string& s) {
  if (s.empty()) return s;
  std::string out = s;
  out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}

// Fixed, cosmetic weekday label for day slot 0..6 (Day 1 = Sunday, Day 2 =
// Monday, ...) -- NOT derived from the real calendar. GymPlanStore's days
// are a repeating cycle the user assigns exercises to themselves (see its
// own header comment: "NOT tied to the calendar"), so this only makes each
// fixed slot easier to refer to than "Day N"; it isn't a claim that Day 1
// falls on an actual Sunday.
const char* weekdayFullName(const int dayIndex) {
  static constexpr StrId kNames[7] = {StrId::STR_WEEKDAY_SUNDAY,   StrId::STR_WEEKDAY_MONDAY,
                                      StrId::STR_WEEKDAY_TUESDAY,  StrId::STR_WEEKDAY_WEDNESDAY,
                                      StrId::STR_WEEKDAY_THURSDAY, StrId::STR_WEEKDAY_FRIDAY,
                                      StrId::STR_WEEKDAY_SATURDAY};
  return I18N.get(kNames[dayIndex % 7]);
}

// Distinct body parts trained on this day, in first-appearance order,
// comma-joined (e.g. "Biceps, Chest") -- a short at-a-glance summary for the
// day card. Capped to whatever's actually assigned, not every body part in
// the catalog.
std::string dayBodyPartsSummary(const WorkoutDay& day) {
  std::vector<std::string> parts;
  for (const auto& ex : day.exercises) {
    if (std::find(parts.begin(), parts.end(), ex.bodyPart) == parts.end()) {
      parts.push_back(ex.bodyPart);
    }
  }
  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i > 0) out += ", ";
    out += capitalize(parts[i]);
  }
  return out;
}

// Day-card grid geometry: 2 columns so a 7-day week reads as a compact
// 2x4 block (last cell empty) instead of a tall single column, matching the
// "apps cover" aesthetic (drawTileCover in RecentBooksActivity.cpp) the user
// asked for -- a solid-black tile with an inset white frame and white text.
constexpr int kGridColumns = 2;
constexpr int kCardGap = 14;
constexpr int kCardCornerRadius = 10;
constexpr int kCardInset = 6;
constexpr int kCardFrameThickness = 2;
}  // namespace

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

  const int dayCount = static_cast<int>(GymPlanStore::DAY_COUNT);
  // Grid has one extra tile after the 7 days: an explicit "Update" tile
  // (selectedIndex_ == dayCount) that runs the same asset sync the long-press
  // Back gesture already did -- that gesture had no visual hint anywhere on
  // this screen, so it was effectively undiscoverable.
  const int tileCount = dayCount + 1;

  // Long-press Confirm: toggle the selected day as a Rest Day (only takes
  // effect when that day has zero exercises -- see GymPlanStore::toggleRestDay).
  // Not meaningful on the Update tile, which isn't a day.
  if (selectedIndex_ < dayCount && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressConfirmFired_ = true;
    GYM_PLAN.toggleRestDay(static_cast<size_t>(selectedIndex_));
    requestUpdate();
    return;
  }

  buttonNavigator_.onScrollNextRelease([this, tileCount] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, tileCount);
    requestUpdate();
  });
  buttonNavigator_.onScrollPreviousRelease([this, tileCount] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, tileCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == dayCount) {
      launchAssetSync();
    } else {
      openSelectedDay();
    }
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
    // +1 for the "Update" tile occupying the slot right after Day 7.
    const int tileCount = dayCount + 1;
    const int rows = (tileCount + kGridColumns - 1) / kGridColumns;

    const int cardWidth = (pageWidth - 2 * metrics.contentSidePadding - (kGridColumns - 1) * kCardGap) / kGridColumns;
    const int cardHeight = (contentHeight - (rows - 1) * kCardGap) / rows;
    const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int topBlockHeight = 14 + smallLineHeight + 6;
    const int bottomBlockHeight = smallLineHeight + 12;

    for (int i = 0; i < dayCount; i++) {
      const int col = i % kGridColumns;
      const int row = i / kGridColumns;
      const int cardX = metrics.contentSidePadding + col * (cardWidth + kCardGap);
      const int cardY = contentTop + row * (cardHeight + kCardGap);

      renderer.fillRoundedRect(cardX, cardY, cardWidth, cardHeight, kCardCornerRadius, Color::Black);
      renderer.drawRoundedRect(cardX + kCardInset, cardY + kCardInset, cardWidth - 2 * kCardInset,
                               cardHeight - 2 * kCardInset, kCardFrameThickness,
                               std::max(1, kCardCornerRadius - kCardInset / 2), false);

      const auto& day = GYM_PLAN.getDay(static_cast<size_t>(i));

      // Weekday name, top.
      const char* weekday = weekdayFullName(i);
      const int weekdayWidth = renderer.getTextWidth(SMALL_FONT_ID, weekday, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, cardX + (cardWidth - weekdayWidth) / 2, cardY + 14, weekday, false,
                        EpdFontFamily::BOLD);

      // Day number, big, centered in the space between the weekday label and
      // the body-part line below.
      char numBuf[4];
      snprintf(numBuf, sizeof(numBuf), "%d", i + 1);
      const int numWidth = renderer.getTextWidth(STOPWATCH_32_FONT_ID, numBuf);
      const int numLineHeight = renderer.getLineHeight(STOPWATCH_32_FONT_ID);
      const int middleTop = cardY + topBlockHeight;
      const int middleBottom = cardY + cardHeight - bottomBlockHeight;
      const int numY = middleTop + std::max(0, (middleBottom - middleTop - numLineHeight) / 2);
      renderer.drawText(STOPWATCH_32_FONT_ID, cardX + (cardWidth - numWidth) / 2, numY, numBuf, false);

      // Body parts trained (or Rest Day / exercise count), bottom.
      std::string subtitle;
      if (day.isRestDay) {
        subtitle = tr(STR_REST_DAY);
      } else if (day.exercises.empty()) {
        char buf[32];
        snprintf(buf, sizeof(buf), tr(STR_EXERCISE_COUNT_FORMAT), 0);
        subtitle = buf;
      } else {
        subtitle = dayBodyPartsSummary(day);
      }
      const std::string truncated =
          renderer.truncatedText(SMALL_FONT_ID, subtitle.c_str(), cardWidth - 2 * kCardInset - 12);
      const int subWidth = renderer.getTextWidth(SMALL_FONT_ID, truncated.c_str());
      renderer.drawText(SMALL_FONT_ID, cardX + (cardWidth - subWidth) / 2, cardY + cardHeight - bottomBlockHeight + 6,
                        truncated.c_str(), false);

      // Selection: thick black outline just outside the card, same
      // convention as RecentBooksActivity's cover grid selection.
      if (i == selectedIndex_) {
        renderer.drawRect(cardX - 4, cardY - 4, cardWidth + 8, cardHeight + 8, 4, true);
      }
    }

    // Extra grid slot right after Day 7: an explicit "Update" tile that syncs
    // missing exercise images/instructions across all 7 days (same action as
    // the long-press Back gesture below, which remains as a shortcut but had
    // no visual hint anywhere on this screen -- this makes it discoverable,
    // and is the natural place for any future Gym-wide sync/update action).
    {
      const int col = dayCount % kGridColumns;
      const int row = dayCount / kGridColumns;
      const int cardX = metrics.contentSidePadding + col * (cardWidth + kCardGap);
      const int cardY = contentTop + row * (cardHeight + kCardGap);

      renderer.fillRoundedRect(cardX, cardY, cardWidth, cardHeight, kCardCornerRadius, Color::Black);
      renderer.drawRoundedRect(cardX + kCardInset, cardY + kCardInset, cardWidth - 2 * kCardInset,
                               cardHeight - 2 * kCardInset, kCardFrameThickness,
                               std::max(1, kCardCornerRadius - kCardInset / 2), false);

      const char* label = tr(STR_UPDATE);
      const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
      const int labelHeight = renderer.getLineHeight(UI_12_FONT_ID);
      renderer.drawText(UI_12_FONT_ID, cardX + (cardWidth - labelWidth) / 2, cardY + (cardHeight - labelHeight) / 2,
                        label, false, EpdFontFamily::BOLD);

      if (dayCount == selectedIndex_) {
        renderer.drawRect(cardX - 4, cardY - 4, cardWidth + 8, cardHeight + 8, 4, true);
      }
    }

    // Long-press action hints, only if the grid leaves spare room below it.
    const int gridBottom = contentTop + rows * cardHeight + (rows - 1) * kCardGap;
    const int hintLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int hintY = gridBottom + metrics.verticalSpacing;
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
