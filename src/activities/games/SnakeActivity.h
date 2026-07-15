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

  // Timing
  unsigned long lastStepMs = 0;
  static constexpr unsigned long STEP_INTERVAL_MS = 300;

  // Score
  int score = 0;

  void initGame();
  void step();
  void spawnFood();
  bool isSnakeAt(int x, int y) const;

  void renderPlaying() const;
  void renderPaused() const;
  void renderGameOver() const;
};
