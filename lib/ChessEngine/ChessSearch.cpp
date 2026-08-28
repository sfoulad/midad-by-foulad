#include "ChessSearch.h"

namespace chess {
namespace {

constexpr int PIECE_VALUES[7] = {0, 100, 320, 330, 500, 900, 0};

// Piece-square tables in centipawns, from White's point of view, indexed
// rank * 8 + file with rank 0 = White's first rank. Black mirrors the rank.
// Values are the classic "simplified evaluation" set: enough to make the engine
// develop, castle and push pawns without a tuning pipeline.
constexpr int8_t PST_PAWN[64] = {0,  0,  0,   0,  0,  0,   0,  0,  5,  10, 10, -20, -20, 10, 10, 5,
                                 5,  -5, -10, 0,  0,  -10, -5, 5,  0,  0,  0,  20,  20,  0,  0,  0,
                                 5,  5,  10,  25, 25, 10,  5,  5,  10, 10, 20, 30,  30,  20, 10, 10,
                                 50, 50, 50,  50, 50, 50,  50, 50, 0,  0,  0,  0,   0,   0,  0,  0};
constexpr int8_t PST_KNIGHT[64] = {-50, -40, -30, -30, -30, -30, -40, -50, -40, -20, 0,   5,   5,   0,   -20, -40,
                                   -30, 5,   10,  15,  15,  10,  5,   -30, -30, 0,   15,  20,  20,  15,  0,   -30,
                                   -30, 5,   15,  20,  20,  15,  5,   -30, -30, 0,   10,  15,  15,  10,  0,   -30,
                                   -40, -20, 0,   0,   0,   0,   -20, -40, -50, -40, -30, -30, -30, -30, -40, -50};
constexpr int8_t PST_BISHOP[64] = {-20, -10, -10, -10, -10, -10, -10, -20, -10, 5,   0,   0,   0,   0,   5,   -10,
                                   -10, 10,  10,  10,  10,  10,  10,  -10, -10, 0,   10,  10,  10,  10,  0,   -10,
                                   -10, 5,   5,   10,  10,  5,   5,   -10, -10, 0,   5,   10,  10,  5,   0,   -10,
                                   -10, 0,   0,   0,   0,   0,   0,   -10, -20, -10, -10, -10, -10, -10, -10, -20};
constexpr int8_t PST_ROOK[64] = {0, 0,  0,  5,  5, 0,  0,  0,  -5, 0,  0,  0, 0, 0, 0, -5, -5, 0,  0,  0, 0, 0,
                                 0, -5, -5, 0,  0, 0,  0,  0,  0,  -5, -5, 0, 0, 0, 0, 0,  0,  -5, -5, 0, 0, 0,
                                 0, 0,  0,  -5, 5, 10, 10, 10, 10, 10, 10, 5, 0, 0, 0, 0,  0,  0,  0,  0};
constexpr int8_t PST_QUEEN[64] = {-20, -10, -10, -5, -5, -10, -10, -20, -10, 0,   5,   0,  0,  0,   0,   -10,
                                  -10, 5,   5,   5,  5,  5,   0,   -10, 0,   0,   5,   5,  5,  5,   0,   -5,
                                  -5,  0,   5,   5,  5,  5,   0,   -5,  -10, 0,   5,   5,  5,  5,   0,   -10,
                                  -10, 0,   0,   0,  0,  0,   0,   -10, -20, -10, -10, -5, -5, -10, -10, -20};
constexpr int8_t PST_KING[64] = {20,  30,  10,  0,   0,   10,  30,  20,  20,  20,  0,   0,   0,   0,   20,  20,
                                 -10, -20, -20, -20, -20, -20, -20, -10, -20, -30, -30, -40, -40, -30, -30, -20,
                                 -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30,
                                 -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30};

const int8_t* pstFor(PieceType t) {
  switch (t) {
    case PAWN:
      return PST_PAWN;
    case KNIGHT:
      return PST_KNIGHT;
    case BISHOP:
      return PST_BISHOP;
    case ROOK:
      return PST_ROOK;
    case QUEEN:
      return PST_QUEEN;
    default:
      return PST_KING;
  }
}

// Nodes between clock reads and between CPU yields. Reading millis() per node
// costs more than the search saves; yielding lets the idle task feed the WDT.
constexpr uint32_t TIME_CHECK_MASK = 1023;
constexpr uint32_t YIELD_MASK = 4095;

}  // namespace

int Search::pieceValue(PieceType t) { return PIECE_VALUES[t]; }

int Search::evaluate(const Board& board) {
  int score = 0;  // positive = good for White
  for (int rank = 0; rank < 8; rank++) {
    for (int file = 0; file < 8; file++) {
      const Piece pc = board.at(squareOf(file, rank));
      if (pc == NO_PIECE) continue;
      const PieceType t = typeOf(pc);
      const bool white = isWhite(pc);
      const int pstIndex = white ? (rank * 8 + file) : ((7 - rank) * 8 + file);
      const int value = PIECE_VALUES[t] + pstFor(t)[pstIndex];
      score += white ? value : -value;
    }
  }
  return board.whiteToMove() ? score : -score;
}

void Search::orderMoves(const Board& board, Move* moves, int count) const {
  // MVV-LVA: try the most valuable victim taken by the least valuable attacker
  // first, then promotions, then quiet moves. Insertion sort -- lists are short
  // and nearly ordered, and it needs no scratch array.
  int scores[MAX_MOVES];
  for (int i = 0; i < count; i++) {
    int s = 0;
    if (moves[i].flags & FLAG_CAPTURE) {
      const Piece victim = board.at(moves[i].to);
      const int victimValue = (moves[i].flags & FLAG_EN_PASSANT) ? PIECE_VALUES[PAWN] : PIECE_VALUES[typeOf(victim)];
      s = 10000 + victimValue * 10 - PIECE_VALUES[typeOf(board.at(moves[i].from))];
    }
    if (moves[i].flags & FLAG_PROMOTION) s += 9000 + PIECE_VALUES[moves[i].promo];
    scores[i] = s;
  }
  for (int i = 1; i < count; i++) {
    const Move m = moves[i];
    const int s = scores[i];
    int j = i - 1;
    while (j >= 0 && scores[j] < s) {
      moves[j + 1] = moves[j];
      scores[j + 1] = scores[j];
      j--;
    }
    moves[j + 1] = m;
    scores[j + 1] = s;
  }
}

bool Search::outOfTime() {
  if (aborted_) return true;
  if (timeUp_) return true;
  if (limitMs_ == 0 || hooks_.nowMs == nullptr) return false;
  if (hooks_.nowMs() - startMs_ >= limitMs_) {
    timeUp_ = true;
    return true;
  }
  return false;
}

int Search::quiesce(Board& board, int ply, int alpha, int beta) {
  nodes_++;
  if ((nodes_ & YIELD_MASK) == 0 && hooks_.yieldCpu != nullptr) hooks_.yieldCpu();
  if ((nodes_ & TIME_CHECK_MASK) == 0 && outOfTime()) return alpha;

  const int standPat = evaluate(board);
  if (ply >= MAX_PLY - 1) return standPat;
  if (standPat >= beta) return beta;
  if (standPat > alpha) alpha = standPat;

  Move* moves = buffer_[ply];
  const int count = board.generateCaptures(moves, MAX_MOVES);
  orderMoves(board, moves, count);

  const bool white = board.whiteToMove();
  for (int i = 0; i < count; i++) {
    Undo undo;
    board.makeMove(moves[i], undo);
    if (board.inCheck(white)) {
      board.unmakeMove(moves[i], undo);
      continue;
    }
    const int score = -quiesce(board, ply + 1, -beta, -alpha);
    board.unmakeMove(moves[i], undo);
    if (score >= beta) return beta;
    if (score > alpha) alpha = score;
  }
  return alpha;
}

int Search::negamax(Board& board, int depth, int ply, int alpha, int beta) {
  nodes_++;
  if ((nodes_ & YIELD_MASK) == 0 && hooks_.yieldCpu != nullptr) hooks_.yieldCpu();
  if ((nodes_ & TIME_CHECK_MASK) == 0 && outOfTime()) return alpha;

  if (depth <= 0 || ply >= MAX_PLY - 4) return quiesce(board, ply, alpha, beta);

  Move* moves = buffer_[ply];
  const int count = board.generateMoves(moves, MAX_MOVES);
  orderMoves(board, moves, count);

  const bool white = board.whiteToMove();
  int legalCount = 0;
  int best = -INFINITE_SCORE;

  for (int i = 0; i < count; i++) {
    Undo undo;
    board.makeMove(moves[i], undo);
    if (board.inCheck(white)) {
      board.unmakeMove(moves[i], undo);
      continue;
    }
    legalCount++;
    const int score = -negamax(board, depth - 1, ply + 1, -beta, -alpha);
    board.unmakeMove(moves[i], undo);

    if (score > best) best = score;
    if (score > alpha) alpha = score;
    if (alpha >= beta) break;
  }

  if (legalCount == 0) {
    // Mate scores shrink with distance so the engine prefers the quicker mate
    // and the longest defence.
    return board.inCheck(white) ? -(MATE_SCORE - ply) : 0;
  }
  return best;
}

Move Search::findBestMove(Board& board, int levelIndex, const Hooks& hooks) {
  hooks_ = hooks;
  nodes_ = 0;
  timeUp_ = false;
  aborted_ = false;

  const LevelConfig& level = LEVELS[(levelIndex < 0) ? 0 : (levelIndex >= LEVEL_COUNT ? LEVEL_COUNT - 1 : levelIndex)];
  limitMs_ = level.timeMs;
  startMs_ = (hooks_.nowMs != nullptr) ? hooks_.nowMs() : 0;

  Move rootMoves[MAX_MOVES];
  const int rootCount = board.generateLegalMoves(rootMoves, MAX_MOVES);
  if (rootCount == 0) return Move{};
  if (rootCount == 1) return rootMoves[0];

  int rootScores[MAX_MOVES] = {};
  Move ordered[MAX_MOVES];
  for (int i = 0; i < rootCount; i++) ordered[i] = rootMoves[i];
  orderMoves(board, ordered, rootCount);

  // Iterative deepening: each completed depth replaces the previous ranking, so
  // a time-out still leaves the last fully searched depth's answer intact.
  for (int i = 0; i < rootCount; i++) rootScores[i] = -INFINITE_SCORE;
  int completedScores[MAX_MOVES] = {};
  for (int i = 0; i < rootCount; i++) completedScores[i] = -INFINITE_SCORE;

  for (int depth = 1; depth <= level.depth; depth++) {
    int alpha = -INFINITE_SCORE;
    bool depthComplete = true;
    for (int i = 0; i < rootCount; i++) {
      Undo undo;
      board.makeMove(ordered[i], undo);
      const int score = -negamax(board, depth - 1, 1, -INFINITE_SCORE, -alpha);
      board.unmakeMove(ordered[i], undo);
      if (outOfTime()) {
        depthComplete = false;
        break;
      }
      rootScores[i] = score;
      if (score > alpha) alpha = score;
    }
    if (!depthComplete) break;
    for (int i = 0; i < rootCount; i++) completedScores[i] = rootScores[i];
    if (outOfTime()) break;
  }

  // Rank by the last completed depth (insertion sort, descending).
  for (int i = 1; i < rootCount; i++) {
    const Move m = ordered[i];
    const int s = completedScores[i];
    int j = i - 1;
    while (j >= 0 && completedScores[j] < s) {
      ordered[j + 1] = ordered[j];
      completedScores[j + 1] = completedScores[j];
      j--;
    }
    ordered[j + 1] = m;
    completedScores[j + 1] = s;
  }

  if (level.blunderPercent > 0 && hooks_.randBelow != nullptr && hooks_.randBelow(100) < level.blunderPercent) {
    // Never blunder away a forced mate the player would obviously see, but do
    // let the weak personas miss it -- that is the point of the level.
    const int pool =
        (level.blunderPool == 0) ? rootCount : (level.blunderPool < rootCount ? level.blunderPool : rootCount);
    return ordered[hooks_.randBelow(static_cast<uint32_t>(pool))];
  }
  return ordered[0];
}

}  // namespace chess
