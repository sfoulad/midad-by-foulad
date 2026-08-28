#pragma once

#include <GfxRenderer.h>

#include "ChessBoard.h"
#include "components/themes/BaseTheme.h"  // Rect

// Board rendering shared by the game and the openings walkthrough: identical
// geometry on both screens, so a position looks the same wherever it appears.
namespace chess_view {

// The two sizes the glyph set is generated at: the board piece, and the small
// one used by the captured-piece strips.
constexpr int GLYPH_SIZE = 42;
constexpr int SMALL_GLYPH_SIZE = 20;

struct Layout {
  int x = 0;
  int y = 0;
  int cellSize = 0;
  int size = 0;  // cellSize * 8
};

// Largest 8x8 board that fits `available`, capped at 56 px cells (the design's
// size on the 480-wide panel), centred horizontally.
Layout computeLayout(const GfxRenderer& renderer, Rect available);

struct Highlights {
  int cursor = -1;    // 0x88 square drawn with the thick cursor border
  int selected = -1;  // 0x88 square of the piece being moved
  int lastFrom = -1;  // previous move, outlined on the walkthrough
  int lastTo = -1;
  const uint8_t* legalTargets = nullptr;  // 64 flags indexed rank * 8 + file
};

// Draws squares, highlights and pieces. `flipped` renders from Black's side.
void drawBoard(const GfxRenderer& renderer, const chess::Board& board, const Layout& layout, const Highlights& hl,
               bool flipped);

// One piece glyph. `size` picks the glyph set: SMALL_GLYPH_SIZE for the
// captured-piece strips, GLYPH_SIZE on the board.
void drawPiece(const GfxRenderer& renderer, chess::Piece piece, int x, int y, int size);

// Pixel top-left of a square, honouring `flipped`.
void squareOrigin(const Layout& layout, uint8_t square, bool flipped, int& outX, int& outY);
// Inverse of squareOrigin: the 0x88 square under a screen point, or -1.
int squareAt(const Layout& layout, int px, int py, bool flipped);

}  // namespace chess_view
