#pragma once

#include <cstdint>
#include <string>

// The reading-session Pomodoro: started from the in-book drawer, displayed in the
// reader's status bar. Phase lengths come from the existing Settings -> Apps ->
// Pomodoro values (pomodoroFocusMin / pomodoroShortBreakMin / pomodoroLongBreakMin),
// so there is one place to set them whether the timer is run from the My Books tile
// or from inside a book.
//
// Deliberately NOT part of StopwatchActivity, which owns the whole screen and can
// redraw as often as it likes. This one is a passenger in the reader's footer, and on
// e-ink every redraw repaints the page under the reader's eyes. So it never asks for a
// refresh: it exposes remainingMs() for whoever is already painting (the reader draws
// it during the repaint a page turn was going to do anyway) and hasJustExpired() as a
// millis()-comparison the reader can poll for free from loop(). The one moment it does
// cost a refresh is the end-of-phase flash, which is the entire point of the feature.
//
// State is process-global rather than owned by the reader activity because activities
// are heap-allocated and deleted on every navigation (see main.cpp): a session would
// otherwise die from opening the drawer's chapter list. Ephemeral by design though --
// nothing is written to SD, so a reboot clears it, matching StopwatchActivity's own
// "resets fresh each time" behaviour.
class ReaderPomodoro {
 public:
  enum class Phase : uint8_t { Focus, ShortBreak, LongBreak };

  static ReaderPomodoro& getInstance();

  // Starts (or restarts) the current phase running from full duration.
  void start();
  // Ends the session entirely and returns to Focus, so the next start is a fresh cycle.
  void stop();
  // Acknowledges a finished phase and starts the next one running. Focus -> break ->
  // Focus, with the long break replacing the short one every
  // POMODORO_CYCLES_BEFORE_LONG_BREAK focus phases.
  void advancePhase();

  // A session exists: either counting down, or finished and waiting to be acknowledged.
  bool isActive() const { return active; }
  // Time is up and the user has not acknowledged it yet.
  bool isFinished() const { return active && finished; }
  Phase currentPhase() const { return phase; }

  uint32_t remainingMs() const;
  // Total length of the current phase, read live from SETTINGS so an edit applies to
  // the next phase rather than needing a restart.
  uint32_t phaseDurationMs() const;

  // True exactly once, on the first call after the running phase's time runs out.
  // Polling this is just a millis() comparison -- cheap enough for the reader's loop()
  // and, crucially, free of any screen refresh. The caller is expected to flash.
  bool consumeJustExpired();

 private:
  ReaderPomodoro() = default;

  bool active = false;
  bool finished = false;
  Phase phase = Phase::Focus;
  // Focus phases completed this session; drives the long-break cadence.
  uint8_t focusesCompleted = 0;
  // millis() when the current phase started counting.
  unsigned long startedAtMs = 0;
};

// MM:SS for a remaining-time value. Shared by the drawer row and the status bar so the
// two readouts of the same countdown can never drift into different formats.
std::string formatPomodoroRemaining(uint32_t ms);

#define READER_POMODORO ReaderPomodoro::getInstance()
