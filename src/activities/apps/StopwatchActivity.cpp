#include "StopwatchActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <string>

#include "HalDisplay.h"
#include "MappedInputManager.h"
#include "MidadAppSettings.h"
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
  resetCycle();
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

uint32_t StopwatchActivity::phaseDurationMs() const {
  // Read live, not cached at onEnter, so a duration edited mid-session applies
  // from the next phase. Clamped to >=1 minute: the Settings ranges start above
  // zero, but these persist to JSON the bidirectional web settings sync can
  // write, and a zero-length phase would expire the instant it started and leave
  // flashAlert() firing in a loop.
  uint8_t minutes = 0;
  switch (phase) {
    case Phase::Focus:
      minutes = MIDAD_APP_SETTINGS.pomodoroFocusMin;
      break;
    case Phase::ShortBreak:
      minutes = MIDAD_APP_SETTINGS.pomodoroShortBreakMin;
      break;
    case Phase::LongBreak:
      minutes = MIDAD_APP_SETTINGS.pomodoroLongBreakMin;
      break;
  }
  if (minutes == 0) minutes = 1;
  return static_cast<uint32_t>(minutes) * 60u * 1000u;
}

uint32_t StopwatchActivity::remainingMs() const {
  const uint32_t total = phaseDurationMs();
  const uint32_t elapsed = elapsedMs();
  return elapsed >= total ? 0 : total - elapsed;
}

void StopwatchActivity::startNextPhase() {
  if (phase == Phase::Focus) {
    ++focusesCompleted;
    phase = (focusesCompleted % MidadAppSettings::POMODORO_CYCLES_BEFORE_LONG_BREAK) == 0 ? Phase::LongBreak
                                                                                          : Phase::ShortBreak;
  } else {
    phase = Phase::Focus;
  }
  running = false;
  phaseFinished = false;
  accumulatedMs = 0;
  startMillis = 0;
}

void StopwatchActivity::resetCycle() {
  phase = Phase::Focus;
  focusesCompleted = 0;
  running = false;
  phaseFinished = false;
  accumulatedMs = 0;
  startMillis = 0;
}

void StopwatchActivity::flashAlert() {
  // The screen is the only alert channel this hardware has -- there is no
  // buzzer, speaker or vibration motor anywhere in the firmware or the SDK. So a
  // finished phase is only noticed if the device is in view; face-down or in a
  // bag it passes silently. That is a hardware ceiling, not something code can
  // work around, and it is why the finished state persists on screen afterwards
  // rather than the flash being the whole notification.
  //
  // Three inverted full-screen passes, same idiom Tasbih uses for its 33/99/100
  // milestones. FULL_REFRESH each way because a partial pass would leave the
  // inversion streaked rather than clean.
  for (int i = 0; i < 3; ++i) {
    renderer.invertScreen();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    delay(180);
    renderer.invertScreen();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    delay(180);
  }
}

void StopwatchActivity::toggleMode() {
  mode = mode == Mode::Stopwatch ? Mode::Pomodoro : Mode::Stopwatch;
  // Each mode starts clean rather than inheriting the other's elapsed time,
  // which would otherwise show a stopwatch reading as a countdown or vice versa.
  running = false;
  accumulatedMs = 0;
  startMillis = 0;
  laps.clear();
  totalLapsRecorded = 0;
  resetCycle();
}

void StopwatchActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleMode();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (mode == Mode::Pomodoro && phaseFinished) {
      startNextPhase();  // acknowledge, and arm the next phase paused
    } else {
      toggleRunning();
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (mode == Mode::Pomodoro) {
      // Running: skip to the next phase. Paused: reset the whole cycle back to
      // the first focus. Mirrors the stopwatch's Lap/Reset split on the same
      // button, so the two modes don't need different muscle memory.
      if (running || phaseFinished) {
        startNextPhase();
      } else {
        resetCycle();
      }
    } else if (running) {
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

  if (!running) return;

  const unsigned long now = millis();

  if (mode == Mode::Pomodoro) {
    if (remainingMs() == 0) {
      running = false;
      phaseFinished = true;
      accumulatedMs = phaseDurationMs();  // pin the display at 00:00
      requestUpdateAndWait();             // paint the finished state, THEN alert on it
      flashAlert();
      return;
    }
    // Coarse tick until the final minute, then per-second. A true 1s tick for a
    // 25-minute phase is ~1500 e-ink refreshes to animate digits nobody is
    // watching; the last minute is the part people actually count down.
    const unsigned long interval = remainingMs() <= 60u * 1000u ? 1000u : 15000u;
    if (now - lastTickMs >= interval) {
      lastTickMs = now;
      requestUpdate();
    }
    return;
  }

  // Live 1s tick so the display keeps counting up while running, without a
  // button press -- same millis()-delta-driven idiom SnakeActivity uses for
  // its step timer.
  if (now - lastTickMs >= 1000) {
    lastTickMs = now;
    requestUpdate();
  }
}

void StopwatchActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  if (mode == Mode::Pomodoro) {
    renderPomodoro(pageWidth, pageHeight);
  } else {
    renderStopwatch(pageWidth, pageHeight);
  }

  renderer.displayBuffer();
}

void StopwatchActivity::renderPomodoro(const int pageWidth, const int pageHeight) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POMODORO));

  // Which phase, and which round of the long-break cadence we're on. The round
  // number is what makes a long break look deliberate rather than random.
  const char* phaseLabel = phase == Phase::Focus        ? tr(STR_POMODORO_FOCUS)
                           : phase == Phase::ShortBreak ? tr(STR_POMODORO_SHORT_BREAK)
                                                        : tr(STR_POMODORO_LONG_BREAK);
  char sub[48];
  snprintf(sub, sizeof(sub), "%s  ·  %s %d", phaseLabel, tr(STR_POMODORO_ROUND),
           (focusesCompleted % MidadAppSettings::POMODORO_CYCLES_BEFORE_LONG_BREAK) + 1);
  renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, sub,
                            true, EpdFontFamily::BOLD);

  constexpr int kNumberFontId = STOPWATCH_32_FONT_ID;
  char timeBuf[16];
  // Counts DOWN. Ceiling, not floor: a phase should read 25:00 the moment it
  // starts, and only reach 00:00 when it is actually over -- flooring would show
  // 24:59 immediately and 00:00 for the whole final second.
  const uint32_t remaining = remainingMs();
  formatElapsed((remaining + 999) / 1000 * 1000, timeBuf, sizeof(timeBuf), /*includeCentiseconds=*/false);
  const int numberWidth = renderer.getTextWidth(kNumberFontId, timeBuf);
  const int numberTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2 + 34;
  renderer.drawText(kNumberFontId, (pageWidth - numberWidth) / 2, numberTop, timeBuf, true);

  const int msgTop = numberTop + renderer.getLineHeight(kNumberFontId) + metrics.verticalSpacing + 12;
  if (phaseFinished) {
    // The flash is gone within a couple of seconds and only works if the device
    // was in view; this line is what a user who looked away comes back to.
    renderer.drawCenteredTextWrapped(UI_10_FONT_ID, msgTop, pageWidth - metrics.contentSidePadding * 2,
                                     phase == Phase::Focus ? tr(STR_POMODORO_FOCUS_DONE) : tr(STR_POMODORO_BREAK_DONE),
                                     /*maxLines=*/2, true, EpdFontFamily::BOLD);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_STOPWATCH), "", "", /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, phaseFinished ? tr(STR_START) : (running ? tr(STR_PAUSE) : tr(STR_START)),
                          running || phaseFinished ? tr(STR_SKIP) : tr(STR_RESET));
}

void StopwatchActivity::renderStopwatch(const int pageWidth, const int pageHeight) {
  const auto& metrics = UITheme::getInstance().getMetrics();

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
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_POMODORO), "", "", /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, startPauseLabel, lapResetLabel);
}
