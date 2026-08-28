#pragma once

#include "ChessBoard.h"

namespace chess {

// A playable game: the board plus everything the UI and the save file need --
// move list in SAN and coordinate form, captured pieces, repetition keys and
// the terminal status. Kept in the engine library (not the activity) so the
// rules that decide a game is over are covered by the host tests.
class Game {
 public:
  enum class Status : uint8_t {
    Playing,
    Check,
    Checkmate,
    Stalemate,
    DrawFiftyMove,
    DrawRepetition,
    DrawMaterial,
    Resigned,
  };

  // A long game is truncated from the front (see appendPly): 200 plies is 100
  // moves each side, past which the oldest plies stop being undoable.
  static constexpr int MAX_PLIES = 200;
  static constexpr int MAX_CAPTURED = 16;

  Game() { reset(); }

  void reset();
  Board& board() { return board_; }
  const Board& board() const { return board_; }

  // Plays a move that must be legal in the current position. Records SAN,
  // coordinate notation, any captured piece and the resulting position key.
  bool play(const Move& m);
  // Convenience for the opening book and the save file.
  bool playSan(const char* san);

  // Takes back one ply. Replays from the base position, so it is O(plies) --
  // irrelevant at this scale and far less error-prone than storing undo state.
  bool undoPly();

  int plyCount() const { return plyCount_; }
  const char* sanAt(int ply) const { return (ply >= 0 && ply < plyCount_) ? san_[ply] : ""; }
  const Move& moveAt(int ply) const { return moves_[ply]; }
  bool hasPlies() const { return plyCount_ > 0; }

  // Pieces the given side has captured, most valuable first.
  const Piece* capturedBy(bool white, int& count) const;
  // Material lead in pawns for `white`, negative when behind.
  int materialLead(bool white) const;

  Status status();
  bool isGameOver() { return status() > Status::Check; }
  void resign() { resigned_ = true; }

  // Save format: base FEN, then space-separated coordinate moves.
  bool toSaveString(char* out, size_t cap) const;
  bool fromSaveString(const char* saved);

 private:
  void appendCaptured(Piece captured);
  void rebuildFromBase();
  int repetitionCount() const;

  Board board_;
  char baseFen_[92] = {};
  Move moves_[MAX_PLIES] = {};
  char san_[MAX_PLIES][10] = {};
  char uci_[MAX_PLIES][6] = {};
  uint32_t keys_[MAX_PLIES + 1] = {};
  Piece capturedWhite_[MAX_CAPTURED] = {};  // black pieces White has taken
  Piece capturedBlack_[MAX_CAPTURED] = {};
  uint8_t capturedWhiteCount_ = 0;
  uint8_t capturedBlackCount_ = 0;
  int plyCount_ = 0;
  bool resigned_ = false;
};

}  // namespace chess
