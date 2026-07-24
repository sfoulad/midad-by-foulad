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
// convert-builtin-fonts.sh) -- both format strings below never produce
// anything else, so it's safe to draw with that font directly.
//
// Centiseconds (hundredths -- a Casio-style stopwatch's third field, not
// literal milliseconds) are only meaningful on a FROZEN reading (paused,
// stopped, or a recorded lap): the e-ink panel's 1-2s full-refresh cost (see
// CLAUDE.md) means the live running display can only redraw ~once/second
// (see loop()'s 1000ms tick), so a hundredths field there would show either
// the real value at whatever sub-second moment that tick's own jitter
// happened to land on -- looking like it jumps between random numbers, not
// counting -- or a value frozen at :00, which looks stuck. Neither reads as
// correct, so render() only passes includeCentiseconds=true for frozen
// readings (paused display, lap list entries); the live running display
// shows MM:SS only, ticking cleanly once a second same as any plain clock.
void formatElapsed(const uint32_t ms, char* buf, const size_t len, const bool includeCentiseconds) {
  const uint32_t totalSeconds = ms / 1000;
  const uint32_t minutes = totalSeconds / 60;
  const uint32_t seconds = totalSeconds % 60;
  if (includeCentiseconds) {
    const uint32_t centiseconds = (ms % 1000) / 10;
    snprintf(buf, len, "%02lu:%02lu:%02lu", static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds),
             static_cast<unsigned long>(centiseconds));
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
  // MM:SS while running (see formatElapsed's comment for why), full
  // MM:SS:CS once paused/stopped -- elapsedMs() is a frozen static value at
  // that point, so real centiseconds are meaningful and exact.
  formatElapsed(elapsedMs(), timeBuf, sizeof(timeBuf), /*includeCentiseconds=*/!running);
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
          formatElapsed(laps[lapCount - 1 - static_cast<size_t>(i)], buf, sizeof(buf), /*includeCentiseconds=*/true);
          return std::string(buf);
        },
        false);
  }

  // Start/Pause and Lap/Reset are the physical SIDE Up/Down buttons (see
  // header comment), not front buttons -- mapLabels()'s previous/next params
  // actually label the front Left/Right buttons (MappedInputManager::mapLabels,
  // keyed off SETTINGS.frontButtonLeft/Right), so passing the action labels
  // there showed them next to buttons that do nothing when pressed. The side
  // buttons have their own dedicated hint widget.
  const char* startPauseLabel = running ? tr(STR_PAUSE) : tr(STR_START);
  const char* lapResetLabel = running ? tr(STR_LAP) : tr(STR_RESET);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "", /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, startPauseLabel, lapResetLabel);

  renderer.displayBuffer();
}
