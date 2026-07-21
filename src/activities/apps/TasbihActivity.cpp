#include "TasbihActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "TasbihStore.h"
#include "activities/stats/AppMetricCard.h"
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
    requestUpdate();
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

  // Big centered count -- BITTER_18_FONT_ID is the largest font this firmware
  // bundles with full ASCII digit coverage (the only larger built-in,
  // SURAHBANNER_24_FONT_ID, only covers a private-use glyph range for surah
  // banners, not real digits).
  constexpr int kNumberFontId = BITTER_18_FONT_ID;
  char countBuf[12];
  snprintf(countBuf, sizeof(countBuf), "%lu", static_cast<unsigned long>(TASBIH.getTodayCount()));
  const int numberWidth = renderer.getTextWidth(kNumberFontId, countBuf, EpdFontFamily::BOLD);
  const int numberY = pageHeight / 2 - renderer.getLineHeight(kNumberFontId) / 2 - metrics.buttonHintsHeight / 2;
  renderer.drawText(kNumberFontId, (pageWidth - numberWidth) / 2, numberY, countBuf, true, EpdFontFamily::BOLD);

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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN), /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
