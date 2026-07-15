#pragma once

// Ported from yattsu/biscuit (github.com/yattsu/biscuit), MIT License,
// Copyright (c) 2025 Dave Allie. Cursor movement now goes through
// ButtonNavigator (logical Up/Down/Left/Right) instead of raw
// MappedInputManager checks, and fires on press instead of release for
// immediate feedback -- same fix applied to Snake's turning. esp_random()
// -> random() for simulator portability. Otherwise unmodified.

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class SudokuActivity final : public Activity {
 public:
  explicit SudokuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sudoku", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum State { PLAYING, SOLVED, NO_SOLUTION };

  ButtonNavigator buttonNavigator_;

  uint8_t board[9][9]{};  // current board (0 = empty)
  bool fixed_[9][9]{};    // true = given clue, not editable
  int cursorX = 0;
  int cursorY = 0;
  State state = PLAYING;

  void generatePuzzle();
  bool solve(uint8_t b[9][9]);
  bool isValid(const uint8_t b[9][9], int row, int col, uint8_t num) const;
};
