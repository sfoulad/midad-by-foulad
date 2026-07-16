#pragma once

// Ported from yattsu/biscuit (github.com/yattsu/biscuit), MIT License,
// Copyright (c) 2025 Dave Allie. Directional input now goes through
// ButtonNavigator (logical Up/Down/Left/Right, same idiom as the My Books
// grid) instead of raw MappedInputManager checks, esp_random() -> random()
// for simulator portability, and a PAUSED state was added (Back pauses
// instead of exiting; everything else is unmodified.

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TetrisActivity final : public Activity {
 public:
  explicit TetrisActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Tetris", renderer, mappedInput) {}

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

  // Board
  static constexpr int BOARD_W = 10;
  static constexpr int BOARD_H = 20;
  int cellSize = 20;
  uint8_t board[BOARD_H][BOARD_W] = {};  // 0 = empty, 1 = filled

  // Piece rendering offset
  int boardOffsetX = 0;
  int boardOffsetY = 0;

  // Tetrominoes: 7 pieces, 4 rotations each, stored as 4x4 bit patterns
  struct Piece {
    uint16_t shape[4];  // 4 rotations, each a 4x4 bitfield (row-major, MSB=top-left)
    // Bits: row0[3:0] row1[3:0] row2[3:0] row3[3:0]
  };

  static const Piece PIECES[7];

  // Current piece
  int currentPiece = 0;
  int currentRotation = 0;
  int pieceX = 0;
  int pieceY = 0;

  // Next piece
  int nextPiece = 0;

  // Score / level
  int score = 0;
  int linesCleared = 0;
  int level = 1;
  // Set once, the moment a run ends (see spawnPiece()'s GAME_OVER branch) --
  // GameHighScoresStore::reportTetrisScore() both persists the new record and
  // tells us whether it was one, same pattern as SnakeActivity.
  bool isNewBest = false;

  // Timing
  unsigned long lastDropMs = 0;
  unsigned long getDropInterval() const;

  // Game logic
  void initGame();
  void spawnPiece();
  bool canPlace(int piece, int rotation, int x, int y) const;
  void lockPiece();
  int clearLines();
  void step();

  // Piece bit access
  static bool getPieceBit(uint16_t shape, int row, int col);

  void renderPlaying() const;
  void renderPaused() const;
  void renderGameOver() const;
  void drawCell(int screenX, int screenY, bool filled) const;

  static int randomPiece();
};
