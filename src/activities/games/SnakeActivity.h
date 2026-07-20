#pragma once

// Ported from yattsu/biscuit (github.com/yattsu/biscuit), MIT License,
// Copyright (c) 2025 Dave Allie. Directional input now goes through
// ButtonNavigator (logical Up/Down/Left/Right, same idiom as the My Books
// grid) instead of raw MappedInputManager checks, esp_random() -> random()
// for simulator portability, and a PAUSED state was added (Back pauses
// instead of exiting; everything else is unmodified.

#include <cstdint>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SnakeActivity final : public Activity {
 public:
  explicit SnakeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Snake", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Only block auto-sleep/power-saving while actually playing -- a paused
  // game shouldn't keep the device awake indefinitely.
  bool preventAutoSleep() override { return state == PLAYING; }
  bool skipLoopDelay() override { return state == PLAYING; }

 private:
  enum State { PLAYING, PAUSED, GAME_OVER };

  State state = PLAYING;

  ButtonNavigator buttonNavigator_;

  // Grid
  static constexpr int CELL_SIZE = 12;
  int gridW = 0;
  int gridH = 0;
  int offsetX = 0;
  int offsetY = 0;

  // Snake
  struct Point {
    int x, y;
  };
  std::vector<Point> snake;
  int dirX = 1, dirY = 0;          // current direction
  int nextDirX = 1, nextDirY = 0;  // buffered next direction

  // Food
  Point food;

  // Timing. Classic Snake rule: the snake speeds up as it grows (each food
  // eaten shortens the step interval), not a fixed cadence for the whole run.
  unsigned long lastStepMs = 0;
  static constexpr unsigned long STEP_INTERVAL_START_MS = 300;
  // Floor chosen with the e-ink panel in mind: HalDisplay's own FAST_REFRESH
  // takes tens of ms, so an interval much below this would have the step
  // timer firing faster than the panel can actually show moves.
  static constexpr unsigned long STEP_INTERVAL_MIN_MS = 90;
  // Interval lost per food eaten; tuned so the floor is reached around
  // length ~30 (300-90)/10 = 21 food eaten -- a satisfying ramp without
  // maxing out speed almost immediately.
  static constexpr unsigned long STEP_INTERVAL_DECAY_MS = 10;
  // Current step interval, recomputed in spawnFood() (i.e. right after
  // growing) so speed reflects the snake's length for the rest of that run.
  unsigned long stepIntervalMs = STEP_INTERVAL_START_MS;

  // Score
  int score = 0;
  // Set once, the moment a run ends (see step()'s collision branches) --
  // GameHighScoresStore::reportSnakeScore() both persists the new record and
  // tells us whether it was one, so renderGameOver() doesn't need to duplicate
  // the comparison against a score that's already been saved.
  bool isNewBest = false;

  void initGame();
  void step();
  void spawnFood();
  bool isSnakeAt(int x, int y) const;

  void renderPlaying() const;
  void renderPaused() const;
  void renderGameOver() const;
};
