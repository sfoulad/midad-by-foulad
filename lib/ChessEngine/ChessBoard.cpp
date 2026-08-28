#include "ChessBoard.h"

#include <cstdio>
#include <cstring>

namespace chess {
namespace {

constexpr int KNIGHT_DELTAS[8] = {31, 33, 14, 18, -31, -33, -14, -18};
constexpr int KING_DELTAS[8] = {1, -1, 16, -16, 15, 17, -15, -17};
constexpr int ROOK_DELTAS[4] = {1, -1, 16, -16};
constexpr int BISHOP_DELTAS[4] = {15, 17, -15, -17};

// Mixing constant for key(): the 32-bit FNV prime.
constexpr uint32_t KEY_PRIME = 16777619u;

constexpr char PIECE_CHARS[7] = {' ', 'P', 'N', 'B', 'R', 'Q', 'K'};

PieceType typeFromChar(char c) {
  switch (c) {
    case 'P':
    case 'p':
      return PAWN;
    case 'N':
    case 'n':
      return KNIGHT;
    case 'B':
    case 'b':
      return BISHOP;
    case 'R':
    case 'r':
      return ROOK;
    case 'Q':
    case 'q':
      return QUEEN;
    case 'K':
    case 'k':
      return KING;
    default:
      return NO_PIECE;
  }
}

}  // namespace

void Board::clear() {
  memset(squares_, 0, sizeof(squares_));
  whiteToMove_ = true;
  castling_ = 0;
  epSquare_ = -1;
  halfmove_ = 0;
  fullmove_ = 1;
  kingSquare_[0] = kingSquare_[1] = 0;
}

void Board::setStartPosition() {
  static constexpr PieceType BACK_RANK[8] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
  clear();
  for (int f = 0; f < 8; f++) {
    squares_[squareOf(f, 0)] = makePiece(BACK_RANK[f], true);
    squares_[squareOf(f, 1)] = makePiece(PAWN, true);
    squares_[squareOf(f, 6)] = makePiece(PAWN, false);
    squares_[squareOf(f, 7)] = makePiece(BACK_RANK[f], false);
  }
  whiteToMove_ = true;
  castling_ = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
  kingSquare_[0] = squareOf(4, 0);
  kingSquare_[1] = squareOf(4, 7);
}

void Board::squareName(uint8_t sq, char* out) {
  out[0] = static_cast<char>('a' + fileOf(sq));
  out[1] = static_cast<char>('1' + rankOf(sq));
  out[2] = '\0';
}

bool Board::fromFen(const char* fen) {
  if (fen == nullptr) return false;
  Board scratch;
  scratch.clear();

  const char* p = fen;
  int rank = 7, file = 0;
  for (; *p != '\0' && *p != ' '; ++p) {
    if (*p == '/') {
      if (file != 8) return false;
      rank--;
      file = 0;
      if (rank < 0) return false;
      continue;
    }
    if (*p >= '1' && *p <= '8') {
      file += *p - '0';
      if (file > 8) return false;
      continue;
    }
    const PieceType t = typeFromChar(*p);
    if (t == NO_PIECE || file > 7) return false;
    const bool white = (*p >= 'A' && *p <= 'Z');
    const uint8_t sq = squareOf(file, rank);
    scratch.squares_[sq] = makePiece(t, white);
    if (t == KING) scratch.kingSquare_[white ? 0 : 1] = sq;
    file++;
  }
  if (rank != 0 || file != 8) return false;

  while (*p == ' ') ++p;
  if (*p != 'w' && *p != 'b') return false;
  scratch.whiteToMove_ = (*p == 'w');
  ++p;

  while (*p == ' ') ++p;
  if (*p == '-') {
    ++p;
  } else {
    for (; *p != '\0' && *p != ' '; ++p) {
      switch (*p) {
        case 'K':
          scratch.castling_ |= CASTLE_WK;
          break;
        case 'Q':
          scratch.castling_ |= CASTLE_WQ;
          break;
        case 'k':
          scratch.castling_ |= CASTLE_BK;
          break;
        case 'q':
          scratch.castling_ |= CASTLE_BQ;
          break;
        default:
          return false;
      }
    }
  }

  while (*p == ' ') ++p;
  if (*p == '-') {
    ++p;
  } else if (*p >= 'a' && *p <= 'h' && p[1] >= '1' && p[1] <= '8') {
    scratch.epSquare_ = static_cast<int8_t>(squareOf(*p - 'a', p[1] - '1'));
    p += 2;
  } else if (*p != '\0') {
    return false;
  }

  while (*p == ' ') ++p;
  if (*p >= '0' && *p <= '9') {
    int v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (*p - '0');
    scratch.halfmove_ = static_cast<uint8_t>(v > 255 ? 255 : v);
  }
  while (*p == ' ') ++p;
  if (*p >= '0' && *p <= '9') {
    int v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (*p - '0');
    scratch.fullmove_ = static_cast<uint16_t>(v < 1 ? 1 : v);
  }

  *this = scratch;
  return true;
}

bool Board::toFen(char* out, size_t cap) const {
  char buf[100];
  size_t n = 0;
  for (int rank = 7; rank >= 0; rank--) {
    int empty = 0;
    for (int file = 0; file < 8; file++) {
      const Piece pc = squares_[squareOf(file, rank)];
      if (pc == NO_PIECE) {
        empty++;
        continue;
      }
      if (empty > 0) buf[n++] = static_cast<char>('0' + empty);
      empty = 0;
      const char c = PIECE_CHARS[typeOf(pc)];
      buf[n++] = isWhite(pc) ? c : static_cast<char>(c - 'A' + 'a');
    }
    if (empty > 0) buf[n++] = static_cast<char>('0' + empty);
    if (rank > 0) buf[n++] = '/';
  }
  buf[n++] = ' ';
  buf[n++] = whiteToMove_ ? 'w' : 'b';
  buf[n++] = ' ';
  if (castling_ == 0) {
    buf[n++] = '-';
  } else {
    if (castling_ & CASTLE_WK) buf[n++] = 'K';
    if (castling_ & CASTLE_WQ) buf[n++] = 'Q';
    if (castling_ & CASTLE_BK) buf[n++] = 'k';
    if (castling_ & CASTLE_BQ) buf[n++] = 'q';
  }
  buf[n++] = ' ';
  if (epSquare_ < 0) {
    buf[n++] = '-';
  } else {
    char sq[3];
    squareName(static_cast<uint8_t>(epSquare_), sq);
    buf[n++] = sq[0];
    buf[n++] = sq[1];
  }
  n += static_cast<size_t>(
      snprintf(buf + n, sizeof(buf) - n, " %u %u", static_cast<unsigned>(halfmove_), static_cast<unsigned>(fullmove_)));
  if (n + 1 > cap) {
    if (cap > 0) out[0] = '\0';
    return false;
  }
  memcpy(out, buf, n);
  out[n] = '\0';
  return true;
}

bool Board::isAttacked(uint8_t sq, bool byWhite) const {
  // A white pawn on sq-15 / sq-17 attacks sq (mirrored for black).
  const int pawnDeltas[2] = {byWhite ? -15 : 15, byWhite ? -17 : 17};
  for (const int d : pawnDeltas) {
    const int from = sq + d;
    if (onBoard(from) && squares_[from] == makePiece(PAWN, byWhite)) return true;
  }
  for (const int d : KNIGHT_DELTAS) {
    const int from = sq + d;
    if (onBoard(from) && squares_[from] == makePiece(KNIGHT, byWhite)) return true;
  }
  for (const int d : KING_DELTAS) {
    const int from = sq + d;
    if (onBoard(from) && squares_[from] == makePiece(KING, byWhite)) return true;
  }
  for (const int d : ROOK_DELTAS) {
    for (int from = sq + d; onBoard(from); from += d) {
      const Piece pc = squares_[from];
      if (pc == NO_PIECE) continue;
      if (isWhite(pc) == byWhite && (typeOf(pc) == ROOK || typeOf(pc) == QUEEN)) return true;
      break;
    }
  }
  for (const int d : BISHOP_DELTAS) {
    for (int from = sq + d; onBoard(from); from += d) {
      const Piece pc = squares_[from];
      if (pc == NO_PIECE) continue;
      if (isWhite(pc) == byWhite && (typeOf(pc) == BISHOP || typeOf(pc) == QUEEN)) return true;
      break;
    }
  }
  return false;
}

bool Board::inCheck(bool white) const { return isAttacked(kingSquare_[white ? 0 : 1], !white); }

void Board::addPawnMoves(Move* out, int& n, int cap, uint8_t from, bool white, bool capturesOnly) const {
  const int forward = white ? 16 : -16;
  const int startRank = white ? 1 : 6;
  const int promoRank = white ? 7 : 0;

  const auto push = [&](uint8_t to, uint8_t flags) {
    if (rankOf(to) == promoRank) {
      static constexpr PieceType PROMO_ORDER[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
      for (const PieceType promo : PROMO_ORDER) {
        if (n >= cap) return;
        out[n++] = Move{from, to, static_cast<uint8_t>(promo), static_cast<uint8_t>(flags | FLAG_PROMOTION)};
      }
    } else if (n < cap) {
      out[n++] = Move{from, to, NO_PIECE, flags};
    }
  };

  const int one = from + forward;
  if (onBoard(one) && squares_[one] == NO_PIECE) {
    // Quiescence still wants promotions: they are forcing, not quiet.
    if (!capturesOnly || rankOf(one) == promoRank) push(static_cast<uint8_t>(one), FLAG_NONE);
    const int two = from + 2 * forward;
    if (!capturesOnly && rankOf(from) == startRank && onBoard(two) && squares_[two] == NO_PIECE && n < cap) {
      out[n++] = Move{from, static_cast<uint8_t>(two), NO_PIECE, FLAG_DOUBLE_PUSH};
    }
  }

  const int captureDeltas[2] = {forward - 1, forward + 1};
  for (const int d : captureDeltas) {
    const int to = from + d;
    if (!onBoard(to)) continue;
    const Piece target = squares_[to];
    if (target != NO_PIECE && isWhite(target) != white) {
      push(static_cast<uint8_t>(to), FLAG_CAPTURE);
    } else if (target == NO_PIECE && epSquare_ >= 0 && to == epSquare_ && n < cap) {
      out[n++] = Move{from, static_cast<uint8_t>(to), NO_PIECE, FLAG_CAPTURE | FLAG_EN_PASSANT};
    }
  }
}

int Board::generateInternal(Move* out, int cap, bool capturesOnly) const {
  const bool white = whiteToMove_;
  int n = 0;

  for (int rank = 0; rank < 8 && n < cap; rank++) {
    for (int file = 0; file < 8 && n < cap; file++) {
      const uint8_t from = squareOf(file, rank);
      const Piece pc = squares_[from];
      if (pc == NO_PIECE || isWhite(pc) != white) continue;
      const PieceType t = typeOf(pc);

      if (t == PAWN) {
        addPawnMoves(out, n, cap, from, white, capturesOnly);
        continue;
      }

      // Returns true when a sliding ray may continue past `to`.
      const auto tryTarget = [&](int to) -> bool {
        if (!onBoard(to)) return false;
        const Piece target = squares_[to];
        if (target == NO_PIECE) {
          if (!capturesOnly && n < cap) out[n++] = Move{from, static_cast<uint8_t>(to), NO_PIECE, FLAG_NONE};
          return true;
        }
        if (isWhite(target) != white && n < cap) {
          out[n++] = Move{from, static_cast<uint8_t>(to), NO_PIECE, FLAG_CAPTURE};
        }
        return false;
      };

      if (t == KNIGHT) {
        for (const int d : KNIGHT_DELTAS) tryTarget(from + d);
      } else if (t == KING) {
        for (const int d : KING_DELTAS) tryTarget(from + d);
        if (!capturesOnly) {
          const uint8_t kingHome = white ? squareOf(4, 0) : squareOf(4, 7);
          const uint8_t kingRight = white ? CASTLE_WK : CASTLE_BK;
          const uint8_t queenRight = white ? CASTLE_WQ : CASTLE_BQ;
          if (from == kingHome && (castling_ & (kingRight | queenRight)) && !isAttacked(kingHome, !white)) {
            if ((castling_ & kingRight) && squares_[kingHome + 1] == NO_PIECE && squares_[kingHome + 2] == NO_PIECE &&
                !isAttacked(static_cast<uint8_t>(kingHome + 1), !white) && n < cap) {
              out[n++] = Move{from, static_cast<uint8_t>(kingHome + 2), NO_PIECE, FLAG_CASTLE_KING};
            }
            if ((castling_ & queenRight) && squares_[kingHome - 1] == NO_PIECE && squares_[kingHome - 2] == NO_PIECE &&
                squares_[kingHome - 3] == NO_PIECE && !isAttacked(static_cast<uint8_t>(kingHome - 1), !white) &&
                n < cap) {
              out[n++] = Move{from, static_cast<uint8_t>(kingHome - 2), NO_PIECE, FLAG_CASTLE_QUEEN};
            }
          }
        }
      } else {
        if (t == ROOK || t == QUEEN) {
          for (const int d : ROOK_DELTAS) {
            for (int to = from + d; tryTarget(to); to += d) {
            }
          }
        }
        if (t == BISHOP || t == QUEEN) {
          for (const int d : BISHOP_DELTAS) {
            for (int to = from + d; tryTarget(to); to += d) {
            }
          }
        }
      }
    }
  }
  return n;
}

int Board::generateMoves(Move* out, int cap) const { return generateInternal(out, cap, false); }

int Board::generateCaptures(Move* out, int cap) const { return generateInternal(out, cap, true); }

int Board::generateLegalMoves(Move* out, int cap) {
  Move pseudo[MAX_MOVES];
  const int count = generateMoves(pseudo, MAX_MOVES);
  const bool white = whiteToMove_;
  int n = 0;
  for (int i = 0; i < count && n < cap; i++) {
    Undo undo;
    makeMove(pseudo[i], undo);
    const bool legal = !inCheck(white);
    unmakeMove(pseudo[i], undo);
    if (legal) out[n++] = pseudo[i];
  }
  return n;
}

bool Board::hasLegalMove() {
  Move pseudo[MAX_MOVES];
  const int count = generateMoves(pseudo, MAX_MOVES);
  const bool white = whiteToMove_;
  for (int i = 0; i < count; i++) {
    Undo undo;
    makeMove(pseudo[i], undo);
    const bool legal = !inCheck(white);
    unmakeMove(pseudo[i], undo);
    if (legal) return true;
  }
  return false;
}

void Board::makeMove(const Move& m, Undo& undo) {
  undo.captured = NO_PIECE;
  undo.capturedSquare = m.to;
  undo.castling = castling_;
  undo.epSquare = epSquare_;
  undo.halfmove = halfmove_;

  const Piece moving = squares_[m.from];
  const bool white = isWhite(moving);
  const PieceType movingType = typeOf(moving);

  if (m.flags & FLAG_EN_PASSANT) {
    undo.capturedSquare = static_cast<uint8_t>(white ? m.to - 16 : m.to + 16);
    undo.captured = squares_[undo.capturedSquare];
    squares_[undo.capturedSquare] = NO_PIECE;
  } else if (squares_[m.to] != NO_PIECE) {
    undo.captured = squares_[m.to];
  }

  squares_[m.to] = (m.flags & FLAG_PROMOTION) ? makePiece(static_cast<PieceType>(m.promo), white) : moving;
  squares_[m.from] = NO_PIECE;

  if (m.flags & FLAG_CASTLE_KING) {
    const uint8_t rookFrom = static_cast<uint8_t>(m.to + 1);
    squares_[m.to - 1] = squares_[rookFrom];
    squares_[rookFrom] = NO_PIECE;
  } else if (m.flags & FLAG_CASTLE_QUEEN) {
    const uint8_t rookFrom = static_cast<uint8_t>(m.to - 2);
    squares_[m.to + 1] = squares_[rookFrom];
    squares_[rookFrom] = NO_PIECE;
  }

  if (movingType == KING) {
    kingSquare_[white ? 0 : 1] = m.to;
    castling_ = static_cast<uint8_t>(castling_ & ~(white ? (CASTLE_WK | CASTLE_WQ) : (CASTLE_BK | CASTLE_BQ)));
  }
  // A move from or to a rook's home square kills that right; the "to" case is
  // how a rook captured on its own square loses it.
  const uint8_t touched[2] = {m.from, m.to};
  for (const uint8_t sq : touched) {
    if (sq == squareOf(7, 0)) castling_ = static_cast<uint8_t>(castling_ & ~CASTLE_WK);
    if (sq == squareOf(0, 0)) castling_ = static_cast<uint8_t>(castling_ & ~CASTLE_WQ);
    if (sq == squareOf(7, 7)) castling_ = static_cast<uint8_t>(castling_ & ~CASTLE_BK);
    if (sq == squareOf(0, 7)) castling_ = static_cast<uint8_t>(castling_ & ~CASTLE_BQ);
  }

  epSquare_ = (m.flags & FLAG_DOUBLE_PUSH) ? static_cast<int8_t>(white ? m.from + 16 : m.from - 16) : -1;
  halfmove_ = (movingType == PAWN || undo.captured != NO_PIECE) ? 0 : static_cast<uint8_t>(halfmove_ + 1);
  if (!white) fullmove_++;
  whiteToMove_ = !whiteToMove_;
}

void Board::unmakeMove(const Move& m, const Undo& undo) {
  whiteToMove_ = !whiteToMove_;
  const bool white = whiteToMove_;
  if (!white) fullmove_--;

  const Piece moved = squares_[m.to];
  squares_[m.from] = (m.flags & FLAG_PROMOTION) ? makePiece(PAWN, white) : moved;
  squares_[m.to] = NO_PIECE;
  if (undo.captured != NO_PIECE) squares_[undo.capturedSquare] = undo.captured;

  if (m.flags & FLAG_CASTLE_KING) {
    squares_[m.to + 1] = squares_[m.to - 1];
    squares_[m.to - 1] = NO_PIECE;
  } else if (m.flags & FLAG_CASTLE_QUEEN) {
    squares_[m.to - 2] = squares_[m.to + 1];
    squares_[m.to + 1] = NO_PIECE;
  }

  if (typeOf(squares_[m.from]) == KING) kingSquare_[white ? 0 : 1] = m.from;

  castling_ = undo.castling;
  epSquare_ = undo.epSquare;
  halfmove_ = undo.halfmove;
}

bool Board::isInsufficientMaterial() const {
  int minors = 0;
  for (int rank = 0; rank < 8; rank++) {
    for (int file = 0; file < 8; file++) {
      const Piece pc = squares_[squareOf(file, rank)];
      if (pc == NO_PIECE) continue;
      switch (typeOf(pc)) {
        case KING:
          break;
        case BISHOP:
        case KNIGHT:
          minors++;
          break;
        default:
          return false;  // a pawn, rook or queen can still mate
      }
    }
  }
  return minors <= 1;
}

uint32_t Board::key() const {
  uint32_t h = 2166136261u;
  for (int rank = 0; rank < 8; rank++) {
    for (int file = 0; file < 8; file++) {
      h = (h ^ static_cast<uint8_t>(squares_[squareOf(file, rank)])) * KEY_PRIME;
    }
  }
  h = (h ^ (whiteToMove_ ? 1u : 2u)) * KEY_PRIME;
  h = (h ^ castling_) * KEY_PRIME;
  h = (h ^ static_cast<uint8_t>(epSquare_ + 1)) * KEY_PRIME;
  return h;
}

bool Board::toSan(const Move& m, char* out, size_t cap) {
  char buf[12];
  size_t n = 0;

  const Piece moving = squares_[m.from];
  if (moving == NO_PIECE) return false;
  const PieceType t = typeOf(moving);

  if (m.flags & FLAG_CASTLE_KING) {
    memcpy(buf, "O-O", 3);
    n = 3;
  } else if (m.flags & FLAG_CASTLE_QUEEN) {
    memcpy(buf, "O-O-O", 5);
    n = 5;
  } else {
    char toName[3];
    squareName(m.to, toName);
    if (t == PAWN) {
      if (m.flags & FLAG_CAPTURE) {
        buf[n++] = static_cast<char>('a' + fileOf(m.from));
        buf[n++] = 'x';
      }
    } else {
      buf[n++] = PIECE_CHARS[t];
      // Disambiguate against other same-type pieces that can legally reach m.to.
      Move legal[MAX_MOVES];
      const int count = generateLegalMoves(legal, MAX_MOVES);
      bool sameFile = false, sameRank = false, ambiguous = false;
      for (int i = 0; i < count; i++) {
        if (legal[i].to != m.to || legal[i].from == m.from) continue;
        if (typeOf(squares_[legal[i].from]) != t) continue;
        ambiguous = true;
        if (fileOf(legal[i].from) == fileOf(m.from)) sameFile = true;
        if (rankOf(legal[i].from) == rankOf(m.from)) sameRank = true;
      }
      if (ambiguous) {
        if (!sameFile) {
          buf[n++] = static_cast<char>('a' + fileOf(m.from));
        } else if (!sameRank) {
          buf[n++] = static_cast<char>('1' + rankOf(m.from));
        } else {
          buf[n++] = static_cast<char>('a' + fileOf(m.from));
          buf[n++] = static_cast<char>('1' + rankOf(m.from));
        }
      }
      if (m.flags & FLAG_CAPTURE) buf[n++] = 'x';
    }
    buf[n++] = toName[0];
    buf[n++] = toName[1];
    if (m.flags & FLAG_PROMOTION) {
      buf[n++] = '=';
      buf[n++] = PIECE_CHARS[m.promo];
    }
  }

  Undo undo;
  makeMove(m, undo);
  const bool opponentInCheck = inCheck(whiteToMove_);
  const bool opponentStuck = !hasLegalMove();
  unmakeMove(m, undo);
  if (opponentInCheck) buf[n++] = opponentStuck ? '#' : '+';

  if (n + 1 > cap) {
    if (cap > 0) out[0] = '\0';
    return false;
  }
  memcpy(out, buf, n);
  out[n] = '\0';
  return true;
}

bool Board::parseSan(const char* san, Move& out) {
  if (san == nullptr || *san == '\0') return false;
  Move legal[MAX_MOVES];
  const int count = generateLegalMoves(legal, MAX_MOVES);

  // Strip decorations so "Nf3!?" and "Nf3+" both match the generated "Nf3".
  char want[12];
  size_t w = 0;
  for (const char* p = san; *p != '\0' && w + 1 < sizeof(want); ++p) {
    if (*p == '+' || *p == '#' || *p == '!' || *p == '?' || *p == ' ') continue;
    want[w++] = *p;
  }
  want[w] = '\0';
  if (w == 0) return false;

  for (int i = 0; i < count; i++) {
    char generated[12];
    if (!toSan(legal[i], generated, sizeof(generated))) continue;
    char plain[12];
    size_t g = 0;
    for (const char* p = generated; *p != '\0'; ++p) {
      if (*p == '+' || *p == '#') continue;
      plain[g++] = *p;
    }
    plain[g] = '\0';
    if (strcmp(plain, want) == 0) {
      out = legal[i];
      return true;
    }
  }

  // Coordinate fallback ("e2e4", "e7e8q") -- the shape the saved history uses.
  if (w >= 4 && want[0] >= 'a' && want[0] <= 'h' && want[1] >= '1' && want[1] <= '8' && want[2] >= 'a' &&
      want[2] <= 'h' && want[3] >= '1' && want[3] <= '8') {
    const uint8_t from = squareOf(want[0] - 'a', want[1] - '1');
    const uint8_t to = squareOf(want[2] - 'a', want[3] - '1');
    const PieceType promo = (w >= 5) ? typeFromChar(want[4]) : QUEEN;
    for (int i = 0; i < count; i++) {
      if (legal[i].from != from || legal[i].to != to) continue;
      if ((legal[i].flags & FLAG_PROMOTION) && legal[i].promo != promo) continue;
      out = legal[i];
      return true;
    }
  }
  return false;
}

}  // namespace chess
