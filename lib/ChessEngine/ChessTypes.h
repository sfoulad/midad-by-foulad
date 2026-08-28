#pragma once

#include <cstdint>

// Core value types for the chess engine. Deliberately free of Arduino/ESP-IDF
// headers so the whole engine builds and runs in the host gtest suite.
namespace chess {

enum PieceType : uint8_t { NO_PIECE = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };

// Board squares hold a signed piece code: positive = white, negative = black,
// magnitude = PieceType. Keeps colour tests to a sign check.
using Piece = int8_t;

constexpr Piece makePiece(PieceType t, bool white) { return static_cast<Piece>(white ? t : -static_cast<int8_t>(t)); }
constexpr PieceType typeOf(Piece p) { return static_cast<PieceType>(p < 0 ? -p : p); }
constexpr bool isWhite(Piece p) { return p > 0; }

// 0x88 square index: rank * 16 + file, a1 == 0. A square is off-board exactly
// when any bit of 0x88 is set, which is the whole point of the layout.
constexpr uint8_t squareOf(int file, int rank) { return static_cast<uint8_t>(rank * 16 + file); }
constexpr int fileOf(int sq) { return sq & 7; }
constexpr int rankOf(int sq) { return sq >> 4; }
constexpr bool onBoard(int sq) { return (sq & 0x88) == 0; }

enum MoveFlag : uint8_t {
  FLAG_NONE = 0,
  FLAG_CAPTURE = 1 << 0,
  FLAG_DOUBLE_PUSH = 1 << 1,
  FLAG_EN_PASSANT = 1 << 2,
  FLAG_CASTLE_KING = 1 << 3,
  FLAG_CASTLE_QUEEN = 1 << 4,
  FLAG_PROMOTION = 1 << 5,
};

struct Move {
  uint8_t from = 0;
  uint8_t to = 0;
  uint8_t promo = NO_PIECE;  // PieceType promoted to, NO_PIECE otherwise
  uint8_t flags = FLAG_NONE;

  bool isNull() const { return from == 0 && to == 0; }
  bool operator==(const Move& o) const { return from == o.from && to == o.to && promo == o.promo && flags == o.flags; }
};

// Castling rights bitmask.
enum CastleRight : uint8_t { CASTLE_WK = 1, CASTLE_WQ = 2, CASTLE_BK = 4, CASTLE_BQ = 8 };

// State a move destroys, kept so unmakeMove is exact.
struct Undo {
  Piece captured = NO_PIECE;
  uint8_t capturedSquare = 0;
  uint8_t castling = 0;
  int8_t epSquare = -1;
  uint8_t halfmove = 0;
};

constexpr int MAX_MOVES = 128;

}  // namespace chess
