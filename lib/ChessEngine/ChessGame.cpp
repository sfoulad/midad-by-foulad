#include "ChessGame.h"

#include <cstdio>
#include <cstring>

namespace chess {
namespace {

constexpr int VALUE_ORDER[7] = {0, 1, 3, 3, 5, 9, 0};

void writeUci(const Move& m, char* out) {
  char from[3], to[3];
  Board::squareName(m.from, from);
  Board::squareName(m.to, to);
  out[0] = from[0];
  out[1] = from[1];
  out[2] = to[0];
  out[3] = to[1];
  if (m.flags & FLAG_PROMOTION) {
    constexpr char PROMO_CHARS[7] = {' ', 'p', 'n', 'b', 'r', 'q', 'k'};
    out[4] = PROMO_CHARS[m.promo];
    out[5] = '\0';
  } else {
    out[4] = '\0';
  }
}

}  // namespace

void Game::reset() {
  board_.setStartPosition();
  board_.toFen(baseFen_, sizeof(baseFen_));
  plyCount_ = 0;
  capturedWhiteCount_ = 0;
  capturedBlackCount_ = 0;
  resigned_ = false;
  keys_[0] = board_.key();
}

void Game::appendCaptured(Piece captured) {
  if (captured == NO_PIECE) return;
  // A captured white piece is one Black took, and vice versa.
  Piece* list = isWhite(captured) ? capturedBlack_ : capturedWhite_;
  uint8_t& count = isWhite(captured) ? capturedBlackCount_ : capturedWhiteCount_;
  if (count >= MAX_CAPTURED) return;
  // Keep the list sorted by value so the UI strip reads queen-first.
  int pos = count;
  while (pos > 0 && VALUE_ORDER[typeOf(list[pos - 1])] < VALUE_ORDER[typeOf(captured)]) {
    list[pos] = list[pos - 1];
    pos--;
  }
  list[pos] = captured;
  count++;
}

bool Game::play(const Move& m) {
  Move legal[MAX_MOVES];
  const int count = board_.generateLegalMoves(legal, MAX_MOVES);
  bool found = false;
  for (int i = 0; i < count; i++) {
    if (legal[i] == m) {
      found = true;
      break;
    }
  }
  if (!found) return false;

  char san[10];
  if (!board_.toSan(m, san, sizeof(san))) return false;

  const Piece captured = (m.flags & FLAG_EN_PASSANT) ? makePiece(PAWN, !board_.whiteToMove()) : board_.at(m.to);

  if (plyCount_ >= MAX_PLIES) {
    // Drop the oldest ply: advance the base position by one and shift the log.
    Board base;
    base.fromFen(baseFen_);
    Undo undo;
    base.makeMove(moves_[0], undo);
    base.toFen(baseFen_, sizeof(baseFen_));
    memmove(moves_, moves_ + 1, sizeof(Move) * (MAX_PLIES - 1));
    memmove(san_, san_ + 1, sizeof(san_[0]) * (MAX_PLIES - 1));
    memmove(uci_, uci_ + 1, sizeof(uci_[0]) * (MAX_PLIES - 1));
    memmove(keys_, keys_ + 1, sizeof(keys_[0]) * MAX_PLIES);
    plyCount_--;
  }

  Undo undo;
  board_.makeMove(m, undo);
  moves_[plyCount_] = m;
  snprintf(san_[plyCount_], sizeof(san_[0]), "%s", san);
  writeUci(m, uci_[plyCount_]);
  plyCount_++;
  keys_[plyCount_] = board_.key();
  appendCaptured(captured);
  return true;
}

bool Game::playSan(const char* san) {
  Move m;
  if (!board_.parseSan(san, m)) return false;
  return play(m);
}

void Game::rebuildFromBase() {
  board_.fromFen(baseFen_);
  capturedWhiteCount_ = 0;
  capturedBlackCount_ = 0;
  keys_[0] = board_.key();
  for (int i = 0; i < plyCount_; i++) {
    const Move& m = moves_[i];
    const Piece captured = (m.flags & FLAG_EN_PASSANT) ? makePiece(PAWN, !board_.whiteToMove()) : board_.at(m.to);
    Undo undo;
    board_.makeMove(m, undo);
    keys_[i + 1] = board_.key();
    appendCaptured(captured);
  }
}

bool Game::undoPly() {
  if (plyCount_ == 0) return false;
  plyCount_--;
  resigned_ = false;
  rebuildFromBase();
  return true;
}

const Piece* Game::capturedBy(bool white, int& count) const {
  count = white ? capturedWhiteCount_ : capturedBlackCount_;
  return white ? capturedWhite_ : capturedBlack_;
}

int Game::materialLead(bool white) const {
  int mine = 0, theirs = 0;
  for (int i = 0; i < capturedWhiteCount_; i++) mine += VALUE_ORDER[typeOf(capturedWhite_[i])];
  for (int i = 0; i < capturedBlackCount_; i++) theirs += VALUE_ORDER[typeOf(capturedBlack_[i])];
  return white ? (mine - theirs) : (theirs - mine);
}

int Game::repetitionCount() const {
  const uint32_t current = keys_[plyCount_];
  int seen = 0;
  for (int i = 0; i <= plyCount_; i++) {
    if (keys_[i] == current) seen++;
  }
  return seen;
}

Game::Status Game::status() {
  if (resigned_) return Status::Resigned;
  const bool inCheck = board_.inCheck(board_.whiteToMove());
  if (!board_.hasLegalMove()) return inCheck ? Status::Checkmate : Status::Stalemate;
  if (board_.isInsufficientMaterial()) return Status::DrawMaterial;
  if (board_.halfmoveClock() >= 100) return Status::DrawFiftyMove;
  if (repetitionCount() >= 3) return Status::DrawRepetition;
  return inCheck ? Status::Check : Status::Playing;
}

bool Game::toSaveString(char* out, size_t cap) const {
  size_t n = 0;
  const size_t baseLen = strlen(baseFen_);
  if (baseLen + 1 > cap) return false;
  memcpy(out, baseFen_, baseLen);
  n = baseLen;
  for (int i = 0; i < plyCount_; i++) {
    const size_t moveLen = strlen(uci_[i]);
    if (n + 1 + moveLen + 1 > cap) break;  // truncating the tail beats failing the whole save
    out[n++] = ' ';
    memcpy(out + n, uci_[i], moveLen);
    n += moveLen;
  }
  out[n] = '\0';
  return true;
}

bool Game::fromSaveString(const char* saved) {
  if (saved == nullptr || *saved == '\0') return false;

  // The FEN is the first six space-separated fields; the moves follow.
  const char* p = saved;
  int spaces = 0;
  while (*p != '\0' && spaces < 6) {
    if (*p == ' ') spaces++;
    if (spaces < 6) p++;
  }
  const size_t fenLen = static_cast<size_t>(p - saved);
  if (fenLen == 0 || fenLen + 1 > sizeof(baseFen_)) return false;

  char fen[92];
  memcpy(fen, saved, fenLen);
  fen[fenLen] = '\0';
  if (!board_.fromFen(fen)) return false;

  memcpy(baseFen_, fen, fenLen + 1);
  plyCount_ = 0;
  capturedWhiteCount_ = 0;
  capturedBlackCount_ = 0;
  resigned_ = false;
  keys_[0] = board_.key();

  while (*p != '\0') {
    while (*p == ' ') p++;
    if (*p == '\0') break;
    char token[8];
    size_t t = 0;
    while (*p != '\0' && *p != ' ' && t + 1 < sizeof(token)) token[t++] = *p++;
    token[t] = '\0';
    while (*p != '\0' && *p != ' ') p++;  // skip any overlong token remainder
    if (!playSan(token)) return false;
  }
  return true;
}

}  // namespace chess
