#include "ReaderPomodoro.h"

#include <Arduino.h>

#include <cstdio>

#include "CrossPointSettings.h"

std::string formatPomodoroRemaining(const uint32_t ms) {
  // Rounded up, so a phase reads its full length the instant it starts (25:00, not
  // 24:59) and only shows 00:00 once the time is genuinely gone.
  const uint32_t totalSeconds = (ms + 999UL) / 1000UL;
  char buf[8];
  snprintf(buf, sizeof(buf), "%lu:%02lu", static_cast<unsigned long>(totalSeconds / 60),
           static_cast<unsigned long>(totalSeconds % 60));
  return buf;
}

ReaderPomodoro& ReaderPomodoro::getInstance() {
  static ReaderPomodoro instance;
  return instance;
}

uint32_t ReaderPomodoro::phaseDurationMs() const {
  // Read live, not cached at start(), so a duration edited mid-session applies from the
  // next phase. Clamped to >=1 minute for the same reason StopwatchActivity clamps it:
  // the Settings ranges start above zero, but these persist to JSON that the
  // bidirectional web settings sync can write anything into, and a zero-length phase
  // would expire the instant it started -- here that would mean a flash on every single
  // page turn.
  uint8_t minutes = 0;
  switch (phase) {
    case Phase::Focus:
      minutes = SETTINGS.pomodoroFocusMin;
      break;
    case Phase::ShortBreak:
      minutes = SETTINGS.pomodoroShortBreakMin;
      break;
    case Phase::LongBreak:
      minutes = SETTINGS.pomodoroLongBreakMin;
      break;
  }
  if (minutes == 0) minutes = 1;
  return static_cast<uint32_t>(minutes) * 60UL * 1000UL;
}

uint32_t ReaderPomodoro::remainingMs() const {
  if (!active || finished) return 0;
  const uint32_t total = phaseDurationMs();
  const uint32_t elapsed = static_cast<uint32_t>(millis() - startedAtMs);
  return elapsed >= total ? 0 : total - elapsed;
}

void ReaderPomodoro::start() {
  active = true;
  finished = false;
  startedAtMs = millis();
}

void ReaderPomodoro::stop() {
  active = false;
  finished = false;
  phase = Phase::Focus;
  focusesCompleted = 0;
  startedAtMs = 0;
}

void ReaderPomodoro::advancePhase() {
  if (phase == Phase::Focus) {
    ++focusesCompleted;
    phase = (focusesCompleted % CrossPointSettings::POMODORO_CYCLES_BEFORE_LONG_BREAK) == 0 ? Phase::LongBreak
                                                                                            : Phase::ShortBreak;
  } else {
    phase = Phase::Focus;
  }
  start();
}

bool ReaderPomodoro::consumeJustExpired() {
  if (!active || finished) return false;
  if (remainingMs() > 0) return false;
  // Latched, so the reader flashes once and the footer then holds the finished state
  // until the user acknowledges it from the drawer. Without the latch this would be
  // true on every loop() tick and flash forever.
  finished = true;
  return true;
}
