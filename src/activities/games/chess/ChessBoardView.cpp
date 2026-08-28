#include "ChessBoardView.h"

#include <algorithm>

#include "components/icons/chessPieces.h"
#include "fontIds.h"

namespace chess_view {
namespace {

using chess::Piece;
using chess::PieceType;

// drawIcon does not scale, so the glyphs exist at exactly two sizes
// (scripts/gen_chess_pieces.py) and the cell size is chosen to fit the big one
// rather than the other way round.
constexpr int MAX_CELL = 56;

struct Glyph {
  const uint8_t* silhouette;
  const uint8_t* ink;
};

Glyph glyphFor(PieceType t, int size) {
  const bool small = (size <= SMALL_GLYPH_SIZE);
  switch (t) {
    case chess::PAWN:
      return small ? Glyph{ChessPawnSilhouette24, ChessPawnInk24} : Glyph{ChessPawnSilhouette42, ChessPawnInk42};
    case chess::KNIGHT:
      return small ? Glyph{ChessKnightSilhouette24, ChessKnightInk24}
                   : Glyph{ChessKnightSilhouette42, ChessKnightInk42};
    case chess::BISHOP:
      return small ? Glyph{ChessBishopSilhouette24, ChessBishopInk24}
                   : Glyph{ChessBishopSilhouette42, ChessBishopInk42};
    case chess::ROOK:
      return small ? Glyph{ChessRookSilhouette24, ChessRookInk24} : Glyph{ChessRookSilhouette42, ChessRookInk42};
    case chess::QUEEN:
      return small ? Glyph{ChessQueenSilhouette24, ChessQueenInk24} : Glyph{ChessQueenSilhouette42, ChessQueenInk42};
    default:
      return small ? Glyph{ChessKingSilhouette24, ChessKingInk24} : Glyph{ChessKingSilhouette42, ChessKingInk42};
  }
}

// File letters and rank numbers, drawn INSIDE the edge squares rather than in a
// margin around the board: the panel has no spare width for a gutter, and
// computeLayout() already spends every pixel it has on the 8 cells.
void drawCoordinates(const GfxRenderer& renderer, const Layout& layout, bool flipped) {
  const int cell = layout.cellSize;
  const int inset = 3;
  const int textHeight = renderer.getLineHeight(SMALL_FONT_ID);

  for (int i = 0; i < 8; i++) {
    const char file[2] = {static_cast<char>('a' + i), '\0'};
    const char rank[2] = {static_cast<char>('1' + i), '\0'};

    // Files run along the bottom edge, labels in the bottom-right of each square;
    // ranks up the left edge, labels in the top-left. Opposite corners so the two
    // never collide in the corner square they share.
    const int col = flipped ? (7 - i) : i;
    const int row = flipped ? (7 - i) : i;  // rank 1 sits at the bottom unflipped

    const int fileX = layout.x + col * cell + cell - inset - renderer.getTextWidth(SMALL_FONT_ID, file);
    const int fileY = layout.y + layout.size - inset - textHeight;
    renderer.drawText(SMALL_FONT_ID, fileX, fileY, file, true);

    const int rankY = layout.y + (7 - row) * cell + inset;
    renderer.drawText(SMALL_FONT_ID, layout.x + inset, rankY, rank, true);
  }
}

}  // namespace

Layout computeLayout(const GfxRenderer& renderer, Rect available) {
  Layout layout;
  layout.cellSize = std::min({available.width / 8, available.height / 8, MAX_CELL});
  if (layout.cellSize < GLYPH_SIZE) {
    // Never shrink below the glyph: a clipped piece is worse than a tighter
    // board, and every supported panel has room for 8 x 42 px.
    layout.cellSize = std::max(layout.cellSize, GLYPH_SIZE);
  }
  layout.size = layout.cellSize * 8;
  layout.x = available.x + (available.width - layout.size) / 2;
  layout.y = available.y;
  return layout;
}

void squareOrigin(const Layout& layout, uint8_t square, bool flipped, int& outX, int& outY) {
  const int file = chess::fileOf(square);
  const int rank = chess::rankOf(square);
  const int col = flipped ? (7 - file) : file;
  const int row = flipped ? rank : (7 - rank);  // rank 8 sits at the top unflipped
  outX = layout.x + col * layout.cellSize;
  outY = layout.y + row * layout.cellSize;
}

int squareAt(const Layout& layout, int px, int py, bool flipped) {
  if (px < layout.x || py < layout.y || px >= layout.x + layout.size || py >= layout.y + layout.size) return -1;
  const int col = (px - layout.x) / layout.cellSize;
  const int row = (py - layout.y) / layout.cellSize;
  const int file = flipped ? (7 - col) : col;
  const int rank = flipped ? row : (7 - row);
  return chess::squareOf(file, rank);
}

void drawPiece(const GfxRenderer& renderer, Piece piece, int x, int y, int size) {
  if (piece == chess::NO_PIECE) return;
  const Glyph glyph = glyphFor(chess::typeOf(piece), size);
  const bool white = chess::isWhite(piece);
  // White pieces are a cleared body plus the ink layer as their outline -- without
  // it they would be an invisible hole. Black pieces are a solid silhouette, and
  // painting the same ink layer in white there cuts the detail lines back OUT of
  // the body, which reads as an outline obscuring the piece rather than defining
  // it. The 1 px board/square rules already separate a black piece from a dark
  // square, so black keeps the silhouette alone.
  renderer.drawIcon(glyph.silhouette, x, y, size, !white);
  if (white) renderer.drawIcon(glyph.ink, x, y, size, true);
}

void drawBoard(const GfxRenderer& renderer, const chess::Board& board, const Layout& layout, const Highlights& hl,
               bool flipped) {
  const int cell = layout.cellSize;

  for (int rank = 0; rank < 8; rank++) {
    for (int file = 0; file < 8; file++) {
      const uint8_t square = chess::squareOf(file, rank);
      int px = 0, py = 0;
      squareOrigin(layout, square, flipped, px, py);

      // Dark squares are a 25% dither, the same texture Sudoku uses for its
      // fixed cells, so the two boards read as one visual family.
      if (((file + rank) & 1) == 0) renderer.fillRectDither(px, py, cell, cell, Color::LightGray);

      if (hl.legalTargets != nullptr && hl.legalTargets[rank * 8 + file] != 0) {
        const Piece target = board.at(square);
        if (target == chess::NO_PIECE) {
          const int dot = std::max(6, cell / 5);
          renderer.fillRect(px + (cell - dot) / 2, py + (cell - dot) / 2, dot, dot, true);
        } else {
          // A capture gets a ring rather than a dot: the dot would sit under
          // the piece already standing there.
          renderer.drawRect(px + 2, py + 2, cell - 4, cell - 4, 3, true);
        }
      }

      const bool lastMove = (square == hl.lastFrom || square == hl.lastTo);
      if (lastMove) renderer.drawRect(px + 1, py + 1, cell - 2, cell - 2, 2, true);

      const Piece piece = board.at(square);
      if (piece != chess::NO_PIECE) {
        drawPiece(renderer, piece, px + (cell - GLYPH_SIZE) / 2, py + (cell - GLYPH_SIZE) / 2, GLYPH_SIZE);
      }

      if (square == hl.selected) renderer.drawRect(px, py, cell, cell, 2, true);
      if (square == hl.cursor) renderer.drawRect(px, py, cell, cell, 3, true);
    }
  }

  renderer.drawRect(layout.x - 1, layout.y - 1, layout.size + 2, layout.size + 2, 1, true);

  drawCoordinates(renderer, layout, flipped);
}

}  // namespace chess_view
