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
  explicit StopwatchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Stopwatch", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Keeps the device awake while actively timing -- an auto-sleep mid-run
  // would silently stop the visible tick and strand the user needing to wake
  // the device just to see elapsed time keep counting.
  bool preventAutoSleep() override { return running; }

 private:
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
