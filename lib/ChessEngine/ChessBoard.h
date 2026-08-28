#pragma once

#include <cstddef>

#include "ChessTypes.h"

namespace chess {

// 0x88 board with make/unmake, legality filtering, FEN and SAN.
// No heap allocation anywhere: every generator writes into a caller-owned buffer.
class Board {
 public:
  Board() { setStartPosition(); }

  void setStartPosition();
  void clear();

  Piece at(uint8_t sq) const { return squares_[sq]; }
  bool whiteToMove() const { return whiteToMove_; }
  uint8_t castling() const { return castling_; }
  int8_t epSquare() const { return epSquare_; }
  uint8_t halfmoveClock() const { return halfmove_; }
  uint16_t fullmoveNumber() const { return fullmove_; }

  bool fromFen(const char* fen);
  // Writes a NUL-terminated FEN; returns false (and leaves out empty) if cap is short.
  bool toFen(char* out, size_t cap) const;

  // Pseudo-legal moves (king may be left in check). Returns the count written.
  int generateMoves(Move* out, int cap) const;
  // Pseudo-legal captures and promotions only -- the quiescence generator.
  int generateCaptures(Move* out, int cap) const;
  // Pseudo-legal moves filtered through make/unmake. Needs a non-const board.
  int generateLegalMoves(Move* out, int cap);

  void makeMove(const Move& m, Undo& undo);
  void unmakeMove(const Move& m, const Undo& undo);

  bool isAttacked(uint8_t sq, bool byWhite) const;
  bool inCheck(bool white) const;
  // Side to move has no legal reply: mate when in check, stalemate otherwise.
  bool hasLegalMove();
  bool isInsufficientMaterial() const;

  // Zobrist-style key over pieces, side, castling and ep. 32 bits is enough for
  // repetition detection in a game this short; collisions cost at worst a
  // spurious draw claim, which the 3-fold rule already tolerates.
  uint32_t key() const;

  // SAN of a move legal in the current position, including disambiguation and
  // the +/# suffix. Mutates and restores the board, hence non-const.
  bool toSan(const Move& m, char* out, size_t cap);
  // Matches SAN (or plain coordinate notation) against the legal move list.
  bool parseSan(const char* san, Move& out);

  static void squareName(uint8_t sq, char* out);  // out needs 3 bytes

 private:
  Piece squares_[128] = {};
  bool whiteToMove_ = true;
  uint8_t castling_ = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
  int8_t epSquare_ = -1;
  uint8_t halfmove_ = 0;
  uint16_t fullmove_ = 1;
  uint8_t kingSquare_[2] = {squareOf(4, 0), squareOf(4, 7)};  // [0]=white, [1]=black

  int generateInternal(Move* out, int cap, bool capturesOnly) const;
  void addPawnMoves(Move* out, int& n, int cap, uint8_t from, bool white, bool capturesOnly) const;
};

}  // namespace chess
