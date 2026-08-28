#pragma once

#include "ChessBoard.h"

namespace chess {

// One opponent persona. Strength comes from search depth plus a deliberate
// blunder rate: a depth-1 engine that never errs still feels inhumanly sharp
// tactically, so the weak levels throw moves away on purpose.
struct LevelConfig {
  uint8_t depth;           // nominal iterative-deepening target
  uint16_t timeMs;         // 0 = no cap (the shallow levels finish instantly)
  uint8_t blunderPercent;  // chance of not playing the best move
  uint8_t blunderPool;     // 0 = any legal move, N = pick among the top N
};

constexpr int LEVEL_COUNT = 5;

// Index 0..4 maps to persona 1..5 (Rashid, Layla, Omar, Nadia, Al-Suli).
constexpr LevelConfig LEVELS[LEVEL_COUNT] = {
    {1, 0, 40, 0}, {2, 0, 15, 3}, {3, 1000, 0, 0}, {4, 2000, 0, 0}, {6, 4000, 0, 0},
};

constexpr int MATE_SCORE = 30000;
constexpr int INFINITE_SCORE = 32000;

// Alpha-beta search with quiescence. Holds its own move buffers so nothing is
// allocated per node and no deep stack frame is needed on the ESP32.
class Search {
 public:
  // Injected so the engine stays free of Arduino headers: the firmware passes
  // millis / esp_random / vTaskDelay, the host tests pass deterministic stubs.
  struct Hooks {
    uint32_t (*nowMs)() = nullptr;
    uint32_t (*randBelow)(uint32_t bound) = nullptr;
    void (*yieldCpu)() = nullptr;
  };

  // Returns a null move only when the side to move has no legal move at all.
  Move findBestMove(Board& board, int levelIndex, const Hooks& hooks);

  // Set from another task to unwind the search early (activity teardown).
  void requestAbort() { aborted_ = true; }
  void clearAbort() { aborted_ = false; }
  bool wasAborted() const { return aborted_; }

  uint32_t nodes() const { return nodes_; }

  // Static evaluation from the side-to-move's point of view, in centipawns.
  static int evaluate(const Board& board);
  static int pieceValue(PieceType t);

 private:
  // depth 6 + 4 quiescence plies + headroom. Each ply owns a move buffer, so
  // this array is the engine's whole working memory (~6 KB).
  static constexpr int MAX_PLY = 12;

  int negamax(Board& board, int depth, int ply, int alpha, int beta);
  int quiesce(Board& board, int ply, int alpha, int beta);
  void orderMoves(const Board& board, Move* moves, int count) const;
  bool outOfTime();

  Move buffer_[MAX_PLY][MAX_MOVES] = {};
  Hooks hooks_{};
  uint32_t nodes_ = 0;
  uint32_t startMs_ = 0;
  uint32_t limitMs_ = 0;
  bool timeUp_ = false;
  volatile bool aborted_ = false;
};

}  // namespace chess
