#pragma once

#include <cstdint>
#include <vector>

#include "activities/Activity.h"

// Simple stopwatch, opened from the "Stop Watch" tile in My Books (see
// STOPWATCH_PSEUDO_PATH in RecentBooksActivity.cpp). Mirrors a classic
// two-pusher chronograph: Up starts/pauses, Down records a lap while running
// or resets while paused -- both the physical, non-remappable side buttons
// (see TasbihActivity's own comment for why). Ephemeral: no persisted store,
// state resets fresh each time the app is opened, like a real stopwatch does
// when put away.
class StopwatchActivity final : public Activity {
 public:
  // Two modes in one activity, not two activities: they share elapsedMs(),
  // toggleRunning(), the tick handling and the render scaffolding, and splitting
  // them would duplicate all of it for no behavioural gain on a device where
  // flash and RAM both matter. Confirm switches between them at runtime; the
  // Pomodoro tile just preselects the second one (see POMODORO_PSEUDO_PATH).
  enum class Mode : uint8_t { Stopwatch, Pomodoro };

  explicit StopwatchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode initialMode = Mode::Stopwatch)
      : Activity("Stopwatch", renderer, mappedInput), mode(initialMode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Keeps the device awake while actively timing -- an auto-sleep mid-run
  // would silently stop the visible tick and strand the user needing to wake
  // the device just to see elapsed time keep counting. Also held while a
  // finished phase waits to be acknowledged: sleeping through the one screen
  // that says the timer ended defeats the alert entirely.
  bool preventAutoSleep() override { return running || phaseFinished; }

 private:
  // Which part of the cycle is running. Long break replaces the short one after
  // every POMODORO_CYCLES_BEFORE_LONG_BREAK focus phases.
  enum class Phase : uint8_t { Focus, ShortBreak, LongBreak };

  Mode mode = Mode::Stopwatch;
  Phase phase = Phase::Focus;
  // Focus phases completed this session; drives the long-break cadence and the
  // round counter on screen. Reset only by an explicit cycle reset.
  int focusesCompleted = 0;
  // True from the moment a phase's time runs out until the user acknowledges it.
  // Deliberately does NOT auto-advance into the next phase: a break that starts
  // itself while you're away from the device is a break you never took.
  bool phaseFinished = false;

  uint32_t phaseDurationMs() const;
  uint32_t remainingMs() const;
  void startNextPhase();
  void resetCycle();
  void flashAlert();
  void toggleMode();
  void renderStopwatch(int pageWidth, int pageHeight);
  void renderPomodoro(int pageWidth, int pageHeight);

  // Ring-buffer cap: kept small enough that every recorded lap fits on screen
  // at once (no scroll capability here -- Up/Down are already Start/Lap and
  // Reset). Older laps are dropped, not the running ordinal count.
  static constexpr size_t kMaxLaps = 8;

  bool running = false;
  unsigned long startMillis = 0;    // millis() when the current run segment began
  unsigned long accumulatedMs = 0;  // elapsed time from all completed run segments
  unsigned long lastTickMs = 0;     // last time we redrew for the live 1s tick

  std::vector<uint32_t> laps;  // elapsed-ms at each recorded lap, oldest first
  int totalLapsRecorded = 0;   // never decreases, even as `laps` evicts old entries

  uint32_t elapsedMs() const;
  void toggleRunning();
  void recordLap();
  void reset();
};
