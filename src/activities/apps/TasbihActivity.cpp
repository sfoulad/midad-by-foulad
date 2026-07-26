#include "TasbihActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "TasbihStore.h"
#include "activities/stats/AppMetricCard.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void TasbihActivity::onEnter() {
  Activity::onEnter();
  // Resets today's count if a day (or year) boundary passed since the app was
  // last open -- must run before the first render, not just before the first
  // increment, or a stale count from a previous day would flash on screen.
  TASBIH.ensureCurrentDay();
  requestUpdate();
}

void TasbihActivity::onExit() {
  // increment() deliberately doesn't save per-tap (SPIFFS write throttling --
  // a dhikr session can mean dozens of taps a minute); persist once here.
  TASBIH.saveToFile();
  Activity::onExit();
}

void TasbihActivity::loop() {
  // Either side button counts -- both are the physical, non-remappable Up/
  // Down buttons, so this always works regardless of front-button remap or
  // orientation, matching how a physical tasbih doesn't care which hand
  // clicks the bead.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    TASBIH.increment();
    // Standard dhikr counts -- a brief inverted flash makes hitting one feel
    // like an actual milestone instead of just another tap.
    const uint32_t count = TASBIH.getTodayCount();
    if (count == 33 || count == 99 || count == 100) {
      milestoneFlash = true;
    }
    requestUpdate();
    return;
  }

  // Confirm is otherwise unused in this activity (Up/Down already own
  // counting) -- reuse it for "start over", the one thing users asked for
  // that a physical tasbih doesn't need (its beads reset by sliding them back
  // by hand). Confirmed via a popup, not a bare press/long-press, since this
  // discards today's progress.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_TASBIH_RESET_TITLE),
                                                                  tr(STR_TASBIH_RESET_BODY)),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) return;
                             TASBIH.resetToday();
                             requestUpdate();
                           });
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void TasbihActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_TASBIH));

  // Big centered count -- TASBIH_32_FONT_ID is a dedicated digit-only 32pt
  // font (see convert-builtin-fonts.sh), bigger than any reader font size;
  // registered as a single "regular" style (the font itself is bold weight).
  constexpr int kNumberFontId = TASBIH_32_FONT_ID;
  char countBuf[12];
  snprintf(countBuf, sizeof(countBuf), "%lu", static_cast<unsigned long>(TASBIH.getTodayCount()));
  const int numberWidth = renderer.getTextWidth(kNumberFontId, countBuf);
  const int numberY = pageHeight / 2 - renderer.getLineHeight(kNumberFontId) / 2 - metrics.buttonHintsHeight / 2;
  if (milestoneFlash) {
    constexpr int kFlashPaddingX = 24;
    constexpr int kFlashPaddingY = 16;
    const int lineHeight = renderer.getLineHeight(kNumberFontId);
    renderer.fillRoundedRect((pageWidth - numberWidth) / 2 - kFlashPaddingX, numberY - kFlashPaddingY,
                             numberWidth + kFlashPaddingX * 2, lineHeight + kFlashPaddingY * 2, 10, Color::Black);
    renderer.drawText(kNumberFontId, (pageWidth - numberWidth) / 2, numberY, countBuf, false);
    milestoneFlash = false;
  } else {
    renderer.drawText(kNumberFontId, (pageWidth - numberWidth) / 2, numberY, countBuf, true);
  }

  // Footer stat cards: Top Tasbih (all-time best day) | Total this year.
  constexpr int kCardHeight = 72;
  const int sidePadding = metrics.contentSidePadding;
  constexpr int kCardGap = 10;
  const int cardWidth = (pageWidth - sidePadding * 2 - kCardGap) / 2;
  const int cardTop = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - kCardHeight;

  char topBuf[12];
  snprintf(topBuf, sizeof(topBuf), "%lu", static_cast<unsigned long>(TASBIH.getMaxSingleDayCount()));
  char yearBuf[12];
  snprintf(yearBuf, sizeof(yearBuf), "%lu", static_cast<unsigned long>(TASBIH.getYearTotal()));

  AppMetricCard::draw(renderer, Rect{sidePadding, cardTop, cardWidth, kCardHeight}, tr(STR_TASBIH_TOP), topBuf);
  AppMetricCard::draw(renderer, Rect{sidePadding + cardWidth + kCardGap, cardTop, cardWidth, kCardHeight},
                      tr(STR_TASBIH_YEAR_TOTAL), yearBuf);

  // The counting action is the physical SIDE Up/Down buttons (see header
  // comment) -- mapLabels()'s previous/next params actually label the front
  // Left/Right buttons (MappedInputManager::mapLabels, keyed off
  // SETTINGS.frontButtonLeft/Right), so STR_DIR_UP/DOWN were showing next to
  // buttons that don't count anything when pressed. The side buttons have
  // their own dedicated hint widget.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RESET), "", "", /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  renderer.displayBuffer();
}
