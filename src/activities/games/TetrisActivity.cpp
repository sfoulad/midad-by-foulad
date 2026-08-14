#include "TetrisActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>

#include "GameHighScoresStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ~12% gray (very light dither) -- used only for the ghost/drop-preview
// piece, which should read as a faint hint of where the piece will land, not
// a filled-in shape competing with the actual board/piece for attention
// (user report: the previous 25% fill looked too heavy/dark). Every other
// row is left fully blank, and the active rows only light 1 pixel in 4.
static void fillDitheredGhost(const GfxRenderer& r, int x, int y, int w, int h) {
  for (int dy = 0; dy < h; dy += 2)
    for (int dx = ((dy / 2) % 2); dx < w; dx += 4) r.drawPixel(x + dx, y + dy, true);
}

// Tetromino shapes encoded as 4x4 bitmaps in a 16-bit value
// Bit layout: bits 15-12 = row 0, bits 11-8 = row 1, bits 7-4 = row 2, bits 3-0 = row 3
// Within each row, bit 3 = col 0 (leftmost), bit 0 = col 3
const TetrisActivity::Piece TetrisActivity::PIECES[7] = {
    // I
    {{0x0F00, 0x2222, 0x00F0, 0x4444}},
    // O
    {{0x6600, 0x6600, 0x6600, 0x6600}},
    // T
    {{0x4E00, 0x4640, 0x0E40, 0x4C40}},
    // S
    {{0x6C00, 0x4620, 0x06C0, 0x8C40}},
    // Z
    {{0xC600, 0x2640, 0x0C60, 0x4C80}},
    // J
    {{0x8E00, 0x6440, 0x0E20, 0x44C0}},
    // L
    {{0x2E00, 0x4460, 0x0E80, 0xC440}},
};

bool TetrisActivity::getPieceBit(uint16_t shape, int row, int col) {
  // row 0 is bits 15-12, row 1 is bits 11-8, etc.
  // col 0 is the highest bit within that nibble
  int bit = 15 - (row * 4 + col);
  return (shape >> bit) & 1;
}

int TetrisActivity::randomPiece() { return static_cast<int>(random(7)); }

void TetrisActivity::onEnter() {
  Activity::onEnter();
  initGame();
}

void TetrisActivity::onExit() { Activity::onExit(); }

void TetrisActivity::initGame() {
  memset(board, 0, sizeof(board));
  score = 0;
  linesCleared = 0;
  level = 1;
  isNewBest = false;
  state = PLAYING;

  const auto& metrics = UITheme::getInstance().getMetrics();
  int screenW = renderer.getScreenWidth();
  int screenH = renderer.getScreenHeight();
  // Dynamic cell size — bounded by available height, and by the side panel's
  // widest element (the 4-cell "NEXT" preview box, sized off this same
  // cellSize) actually fitting in what's left of the width after the board.
  // The old formula just reserved a flat 70% of screen width for the board
  // (sized purely off BOARD_W) and never checked whether the remaining 30%
  // still fit the NEXT box -- confirmed cut off on X3 (528px): that formula
  // picked cellSize=36 (NEXT box needs 4*36+4=148px) with only ~138px left
  // after the board, clipping it off the right edge. Solving cellSize
  // directly against the full layout (board + panel gap + NEXT box + right
  // margin <= screenW) keeps the whole panel on-screen at every width.
  constexpr int kBoardOffsetX = 15;
  constexpr int kPanelGap = 15;
  constexpr int kNextBoxBorder = 4;
  constexpr int kRightMargin = 10;
  int availH = screenH - metrics.topPadding - 25 - metrics.buttonHintsHeight;
  int availW = screenW - kBoardOffsetX - kPanelGap - kNextBoxBorder - kRightMargin;
  int widthConstrainedCell = availW / (BOARD_W + 4);  // board columns + NEXT box's 4 columns
  cellSize = std::min(availH / BOARD_H, widthConstrainedCell);
  if (cellSize < 15) cellSize = 15;
  if (cellSize > 37) cellSize = 37;
  int boardPixelW = BOARD_W * cellSize;
  int boardPixelH = BOARD_H * cellSize;
  // Board on the left side, leaving room for side panel
  boardOffsetX = kBoardOffsetX;
  boardOffsetY = metrics.topPadding + 25;
  (void)boardPixelW;
  (void)boardPixelH;

  nextPiece = randomPiece();
  spawnPiece();
  lastDropMs = millis();
  requestUpdate();
}

void TetrisActivity::spawnPiece() {
  currentPiece = nextPiece;
  nextPiece = randomPiece();
  currentRotation = 0;
  pieceX = BOARD_W / 2 - 2;
  pieceY = 0;

  if (!canPlace(currentPiece, currentRotation, pieceX, pieceY)) {
    state = GAME_OVER;
    isNewBest = GAME_SCORES.reportTetrisScore(static_cast<uint32_t>(score));
  }
}

bool TetrisActivity::canPlace(int piece, int rotation, int x, int y) const {
  uint16_t shape = PIECES[piece].shape[rotation];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!getPieceBit(shape, r, c)) continue;
      int bx = x + c;
      int by = y + r;
      if (bx < 0 || bx >= BOARD_W || by >= BOARD_H) return false;
      if (by >= 0 && board[by][bx]) return false;
    }
  }
  return true;
}

void TetrisActivity::lockPiece() {
  uint16_t shape = PIECES[currentPiece].shape[currentRotation];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!getPieceBit(shape, r, c)) continue;
      int bx = pieceX + c;
      int by = pieceY + r;
      if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W) {
        board[by][bx] = 1;
      }
    }
  }
}

int TetrisActivity::clearLines() {
  int cleared = 0;
  for (int y = BOARD_H - 1; y >= 0; y--) {
    bool full = true;
    for (int x = 0; x < BOARD_W; x++) {
      if (!board[y][x]) {
        full = false;
        break;
      }
    }
    if (full) {
      cleared++;
      // Shift everything above down
      for (int yy = y; yy > 0; yy--) {
        memcpy(board[yy], board[yy - 1], BOARD_W);
      }
      memset(board[0], 0, BOARD_W);
      y++;  // re-check this row
    }
  }
  return cleared;
}

unsigned long TetrisActivity::getDropInterval() const {
  // Speed increases with level
  unsigned long base = 800;
  unsigned long reduction = static_cast<unsigned long>(level - 1) * 70;
  if (reduction >= base - 100) return 100;
  return base - reduction;
}

void TetrisActivity::step() {
  if (canPlace(currentPiece, currentRotation, pieceX, pieceY + 1)) {
    pieceY++;
  } else {
    lockPiece();
    int lines = clearLines();
    if (lines > 0) {
      // Scoring: 1=100, 2=300, 3=500, 4=800
      static const int lineScores[] = {0, 100, 300, 500, 800};
      score += lineScores[lines] * level;
      linesCleared += lines;
      level = 1 + linesCleared / 10;
    }
    spawnPiece();
  }
  requestUpdate();
}

void TetrisActivity::loop() {
  if (state == GAME_OVER) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      initGame();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == PAUSED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Resume: re-baseline the drop timer so the real time spent paused
      // doesn't count as elapsed drop time (which would otherwise slam the
      // piece down through the board the instant play resumes).
      state = PLAYING;
      lastDropMs = millis();
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  // Movement/soft-drop repeat while held; rotate and hard-drop stay
  // single-press. Routed through ButtonNavigator's logical Left/Right/Down
  // (same idiom as the My Books grid) instead of raw MappedInputManager
  // checks, so remapped/orientation-swapped buttons still move the piece
  // correctly.
  buttonNavigator_.onRelease({MappedInputManager::Button::Left}, [this] {
    if (canPlace(currentPiece, currentRotation, pieceX - 1, pieceY)) {
      pieceX--;
      requestUpdate();
    }
  });
  buttonNavigator_.onContinuous({MappedInputManager::Button::Left}, [this] {
    if (canPlace(currentPiece, currentRotation, pieceX - 1, pieceY)) {
      pieceX--;
      requestUpdate();
    }
  });
  buttonNavigator_.onRelease({MappedInputManager::Button::Right}, [this] {
    if (canPlace(currentPiece, currentRotation, pieceX + 1, pieceY)) {
      pieceX++;
      requestUpdate();
    }
  });
  buttonNavigator_.onContinuous({MappedInputManager::Button::Right}, [this] {
    if (canPlace(currentPiece, currentRotation, pieceX + 1, pieceY)) {
      pieceX++;
      requestUpdate();
    }
  });
  buttonNavigator_.onRelease({MappedInputManager::Button::Down}, [this] {
    // Soft drop
    if (canPlace(currentPiece, currentRotation, pieceX, pieceY + 1)) {
      pieceY++;
      score += 1;
      requestUpdate();
    }
  });
  buttonNavigator_.onContinuous({MappedInputManager::Button::Down}, [this] {
    if (canPlace(currentPiece, currentRotation, pieceX, pieceY + 1)) {
      pieceY++;
      score += 1;
      requestUpdate();
    }
  });
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    // Rotate
    int newRot = (currentRotation + 1) % 4;
    if (canPlace(currentPiece, newRot, pieceX, pieceY)) {
      currentRotation = newRot;
      requestUpdate();
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Hard drop
    while (canPlace(currentPiece, currentRotation, pieceX, pieceY + 1)) {
      pieceY++;
      score += 2;
    }
    lockPiece();
    int lines = clearLines();
    if (lines > 0) {
      static const int lineScores[] = {0, 100, 300, 500, 800};
      score += lineScores[lines] * level;
      linesCleared += lines;
      level = 1 + linesCleared / 10;
    }
    spawnPiece();
    lastDropMs = millis();
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    state = PAUSED;
    requestUpdate();
    return;
  }

  // Auto-drop timer
  unsigned long now = millis();
  if (now - lastDropMs >= getDropInterval()) {
    lastDropMs = now;
    step();
  }
}

void TetrisActivity::drawCell(int screenX, int screenY, bool filled) const {
  if (filled) {
    renderer.fillRect(screenX, screenY, cellSize, cellSize, true);
    // White border inside for block definition
    renderer.drawRect(screenX + 1, screenY + 1, cellSize - 2, cellSize - 2, false);
  } else {
    renderer.drawRect(screenX, screenY, cellSize, cellSize);
  }
}

void TetrisActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (state) {
    case PLAYING:
      renderPlaying();
      break;
    case PAUSED:
      renderPlaying();
      renderPaused();
      break;
    case GAME_OVER:
      renderGameOver();
      break;
  }

  renderer.displayBuffer();
}

void TetrisActivity::renderPlaying() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int screenW = renderer.getScreenWidth();

  // Compact header
  renderer.drawText(UI_10_FONT_ID, 15, metrics.topPadding, "TETRIS", true, EpdFontFamily::BOLD);

  // Board border
  int bw = BOARD_W * cellSize;
  int bh = BOARD_H * cellSize;
  // Double-line border
  renderer.drawRect(boardOffsetX - 1, boardOffsetY - 1, bw + 2, bh + 2);
  renderer.drawRect(boardOffsetX - 3, boardOffsetY - 3, bw + 6, bh + 6);

  // Draw board
  for (int y = 0; y < BOARD_H; y++) {
    for (int x = 0; x < BOARD_W; x++) {
      if (board[y][x]) {
        int sx = boardOffsetX + x * cellSize;
        int sy = boardOffsetY + y * cellSize;
        renderer.fillRect(sx, sy, cellSize, cellSize, true);
        renderer.drawRect(sx + 1, sy + 1, cellSize - 2, cellSize - 2, false);
      }
    }
  }

  // Draw current piece
  uint16_t shape = PIECES[currentPiece].shape[currentRotation];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!getPieceBit(shape, r, c)) continue;
      int bx = pieceX + c;
      int by = pieceY + r;
      if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W) {
        int sx = boardOffsetX + bx * cellSize;
        int sy = boardOffsetY + by * cellSize;
        renderer.fillRect(sx, sy, cellSize, cellSize, true);
        renderer.drawRect(sx + 1, sy + 1, cellSize - 2, cellSize - 2, false);
      }
    }
  }

  // Ghost piece (drop preview) - draw outline only
  int ghostY = pieceY;
  while (canPlace(currentPiece, currentRotation, pieceX, ghostY + 1)) {
    ghostY++;
  }
  if (ghostY != pieceY) {
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        if (!getPieceBit(shape, r, c)) continue;
        int bx = pieceX + c;
        int by = ghostY + r;
        if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W) {
          int sx = boardOffsetX + bx * cellSize;
          int sy = boardOffsetY + by * cellSize;
          fillDitheredGhost(renderer, sx, sy, cellSize, cellSize);
          renderer.drawRect(sx, sy, cellSize, cellSize, true);
        }
      }
    }
  }

  // Side panel — right of board
  int panelX = boardOffsetX + bw + 15;
  int panelY = boardOffsetY;
  int panelW = screenW - panelX - 10;
  (void)panelW;

  // Score
  char scoreBuf2[32];
  snprintf(scoreBuf2, sizeof(scoreBuf2), "%d", score);
  renderer.drawText(SMALL_FONT_ID, panelX, panelY, "SCORE");
  renderer.drawText(UI_12_FONT_ID, panelX, panelY + 14, scoreBuf2, true, EpdFontFamily::BOLD);

  // Level
  panelY += 50;
  char levelBuf[16];
  snprintf(levelBuf, sizeof(levelBuf), "%d", level);
  renderer.drawText(SMALL_FONT_ID, panelX, panelY, "LEVEL");
  renderer.drawText(UI_12_FONT_ID, panelX, panelY + 14, levelBuf, true, EpdFontFamily::BOLD);

  // Lines
  panelY += 50;
  char linesBuf2[16];
  snprintf(linesBuf2, sizeof(linesBuf2), "%d", linesCleared);
  renderer.drawText(SMALL_FONT_ID, panelX, panelY, "LINES");
  renderer.drawText(UI_12_FONT_ID, panelX, panelY + 14, linesBuf2, true, EpdFontFamily::BOLD);

  // Next piece
  panelY += 55;
  renderer.drawText(SMALL_FONT_ID, panelX, panelY, "NEXT");
  panelY += 16;
  // Draw next piece at full cell size
  renderer.drawRect(panelX, panelY, cellSize * 4 + 4, cellSize * 4 + 4);
  uint16_t nextShape = PIECES[nextPiece].shape[0];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!getPieceBit(nextShape, r, c)) continue;
      int sx = panelX + 2 + c * cellSize;
      int sy = panelY + 2 + r * cellSize;
      renderer.fillRect(sx, sy, cellSize, cellSize, true);
      renderer.drawRect(sx + 1, sy + 1, cellSize - 2, cellSize - 2, false);
    }
  }

  // Best (all-time high score) -- placed after Next rather than interleaved
  // with Score/Level/Lines so it can't shift their established positions.
  panelY += cellSize * 4 + 4 + 12;
  char bestBuf2[32];
  snprintf(bestBuf2, sizeof(bestBuf2), "%u", GAME_SCORES.getTetrisHighScore());
  renderer.drawText(SMALL_FONT_ID, panelX, panelY, "BEST");
  renderer.drawText(UI_12_FONT_ID, panelX, panelY + 14, bestBuf2, true, EpdFontFamily::BOLD);

  // Back pauses (labeled "Pause" rather than "Exit" -- matches Maze/Snake's
  // pause-not-quit convention). Left/Right move the piece horizontally (Up/Down
  // rotate/soft-drop via the side buttons, which never get hint-bar labels in
  // this app -- same convention as Snake/Sudoku/Maze). Confirm hard-drops.
  const auto labels = mappedInput.mapLabels(tr(STR_PAUSE), tr(STR_DROP), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TetrisActivity::renderPaused() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Dim the frozen board behind a centered panel so it's clear play is
  // suspended, not lost.
  const int panelW = pageWidth * 3 / 4;
  const int panelH = 120;
  const int panelX = (pageWidth - panelW) / 2;
  const int panelY = (pageHeight - panelH) / 2;
  renderer.fillRect(panelX, panelY, panelW, panelH, false);
  renderer.drawRect(panelX, panelY, panelW, panelH);
  renderer.drawRect(panelX + 2, panelY + 2, panelW - 4, panelH - 4);

  renderer.drawCenteredText(UI_12_FONT_ID, panelY + 20, tr(STR_PAUSED), true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_QUIT), tr(STR_RESUME), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TetrisActivity::renderGameOver() const {
  const auto pageHeight = renderer.getScreenHeight();
  int y = pageHeight / 2 - 40;

  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_GAME_OVER), true, EpdFontFamily::BOLD);
  y += 40;

  char scoreBuf[48];
  snprintf(scoreBuf, sizeof(scoreBuf), "Score: %d  Lines: %d", score, linesCleared);
  renderer.drawCenteredText(UI_10_FONT_ID, y, scoreBuf);
  y += 22;

  char bestBuf[32];
  if (isNewBest) {
    snprintf(bestBuf, sizeof(bestBuf), "New Best!");
  } else {
    snprintf(bestBuf, sizeof(bestBuf), "Best: %u", GAME_SCORES.getTetrisHighScore());
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, bestBuf);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
