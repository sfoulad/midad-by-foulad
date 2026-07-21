#include "StopwatchActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// STOPWATCH_32_FONT_ID only has glyphs for '0'-'9' and ':' (see
// convert-builtin-fonts.sh) -- this format string never produces anything
// else, so it's safe to draw with that font directly.
void formatElapsed(const uint32_t ms, char* buf, const size_t len) {
  const uint32_t totalSeconds = ms / 1000;
  const uint32_t hours = totalSeconds / 3600;
  const uint32_t minutes = (totalSeconds % 3600) / 60;
  const uint32_t seconds = totalSeconds % 60;
  if (hours > 0) {
    snprintf(buf, len, "%lu:%02lu:%02lu", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  } else {
    snprintf(buf, len, "%02lu:%02lu", static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
  }
}
}  // namespace

void StopwatchActivity::onEnter() {
  Activity::onEnter();
  running = false;
  startMillis = 0;
  accumulatedMs = 0;
  lastTickMs = millis();
  laps.clear();
  laps.reserve(kMaxLaps);
  totalLapsRecorded = 0;
  requestUpdate();
}

uint32_t StopwatchActivity::elapsedMs() const {
  return static_cast<uint32_t>(accumulatedMs + (running ? millis() - startMillis : 0));
}

void StopwatchActivity::toggleRunning() {
  if (running) {
    accumulatedMs += millis() - startMillis;
    running = false;
  } else {
    startMillis = millis();
    running = true;
  }
}

void StopwatchActivity::recordLap() {
  if (!running) return;
  if (laps.size() >= kMaxLaps) {
    laps.erase(laps.begin());
  }
  laps.push_back(elapsedMs());
  ++totalLapsRecorded;
}

void StopwatchActivity::reset() {
  if (running) return;
  accumulatedMs = 0;
  laps.clear();
  totalLapsRecorded = 0;
}

void StopwatchActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    toggleRunning();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (running) {
      recordLap();
    } else {
      reset();
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Live 1s tick so the display keeps counting up while running, without a
  // button press -- same millis()-delta-driven idiom SnakeActivity uses for
  // its step timer.
  if (running) {
    const unsigned long now = millis();
    if (now - lastTickMs >= 1000) {
      lastTickMs = now;
      requestUpdate();
    }
  }
}

void StopwatchActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_STOPWATCH));

  constexpr int kNumberFontId = STOPWATCH_32_FONT_ID;
  char timeBuf[16];
  formatElapsed(elapsedMs(), timeBuf, sizeof(timeBuf));
  const int numberWidth = renderer.getTextWidth(kNumberFontId, timeBuf);
  const int numberTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 16;
  renderer.drawText(kNumberFontId, (pageWidth - numberWidth) / 2, numberTop, timeBuf, true);

  const int listTop = numberTop + renderer.getLineHeight(kNumberFontId) + metrics.verticalSpacing + 16;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (laps.empty()) {
    const int msgWidth = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_NO_LAPS_YET));
    renderer.drawText(UI_10_FONT_ID, (pageWidth - msgWidth) / 2, listTop + 20, tr(STR_NO_LAPS_YET));
  } else {
    const size_t lapCount = laps.size();
    GUI.drawList(
        renderer, Rect{0, listTop, pageWidth, listHeight}, static_cast<int>(lapCount), -1,
        [this, lapCount](int i) {
          char buf[24];
          snprintf(buf, sizeof(buf), "%s %d", tr(STR_LAP), totalLapsRecorded - i);
          return std::string(buf);
        },
        nullptr, nullptr,
        [this, lapCount](int i) {
          char buf[16];
          formatElapsed(laps[lapCount - 1 - static_cast<size_t>(i)], buf, sizeof(buf));
          return std::string(buf);
        },
        false);
  }

  const char* startPauseLabel = running ? tr(STR_PAUSE) : tr(STR_START);
  const char* lapResetLabel = running ? tr(STR_LAP) : tr(STR_RESET);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", startPauseLabel, lapResetLabel, /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
