#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "ChessBoard.h"
#include "ChessGame.h"
#include "ChessSearch.h"
#include "activities/games/chess/OpeningMoves.h"

using namespace chess;

namespace {

// Perft is the standard move-generator correctness check: it exercises
// castling, en passant, promotion and pin legality all at once, and any
// deviation from the published node counts is a generator bug.
uint64_t perft(Board& board, int depth) {
  if (depth == 0) return 1;
  Move moves[MAX_MOVES];
  const int count = board.generateMoves(moves, MAX_MOVES);
  const bool white = board.whiteToMove();
  uint64_t nodes = 0;
  for (int i = 0; i < count; i++) {
    Undo undo;
    board.makeMove(moves[i], undo);
    if (!board.inCheck(white)) nodes += perft(board, depth - 1);
    board.unmakeMove(moves[i], undo);
  }
  return nodes;
}

Search::Hooks noHooks() { return Search::Hooks{}; }

}  // namespace

TEST(ChessBoard, PerftFromStartPosition) {
  Board board;
  EXPECT_EQ(perft(board, 1), 20u);
  EXPECT_EQ(perft(board, 2), 400u);
  EXPECT_EQ(perft(board, 3), 8902u);
  EXPECT_EQ(perft(board, 4), 197281u);
}

TEST(ChessBoard, PerftKiwipete) {
  // Kiwipete: dense with castling, pins and en-passant edge cases.
  Board board;
  ASSERT_TRUE(board.fromFen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"));
  EXPECT_EQ(perft(board, 1), 48u);
  EXPECT_EQ(perft(board, 2), 2039u);
  EXPECT_EQ(perft(board, 3), 97862u);
}

TEST(ChessBoard, PerftEnPassantAndPromotionPosition) {
  Board board;
  ASSERT_TRUE(board.fromFen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"));
  EXPECT_EQ(perft(board, 1), 14u);
  EXPECT_EQ(perft(board, 2), 191u);
  EXPECT_EQ(perft(board, 3), 2812u);
  EXPECT_EQ(perft(board, 4), 43238u);

  Board promo;
  ASSERT_TRUE(promo.fromFen("n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1"));
  EXPECT_EQ(perft(promo, 1), 24u);
  EXPECT_EQ(perft(promo, 3), 9483u);
}

TEST(ChessBoard, FenRoundTrip) {
  const char* fens[] = {
      "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 b - e3 5 42",
  };
  for (const char* fen : fens) {
    Board board;
    ASSERT_TRUE(board.fromFen(fen)) << fen;
    char out[100];
    ASSERT_TRUE(board.toFen(out, sizeof(out)));
    EXPECT_STREQ(out, fen);
  }
}

TEST(ChessBoard, SanGenerationCoversDisambiguationAndSuffixes) {
  Board board;
  ASSERT_TRUE(board.fromFen("r1bqkb1r/pppp1Qpp/2n2n2/4p3/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 0 4"));
  // Only the king can move, and it cannot take the queen (bishop defends).
  Move legal[MAX_MOVES];
  EXPECT_EQ(board.generateLegalMoves(legal, MAX_MOVES), 0);

  Board twoKnights;
  ASSERT_TRUE(twoKnights.fromFen("7k/8/8/8/8/8/8/N3N2K w - - 0 1"));
  Move m;
  ASSERT_TRUE(twoKnights.parseSan("Nac2", m));
  char san[12];
  ASSERT_TRUE(twoKnights.toSan(m, san, sizeof(san)));
  EXPECT_STREQ(san, "Nac2");

  // A check the king can walk out of: "+" rather than "#".
  Board mateIn1;
  ASSERT_TRUE(mateIn1.fromFen("6k1/5p1p/6p1/8/8/8/8/R5K1 w - - 0 1"));
  ASSERT_TRUE(mateIn1.parseSan("Ra8+", m));
  ASSERT_TRUE(mateIn1.toSan(m, san, sizeof(san)));
  EXPECT_STREQ(san, "Ra8+");
}

TEST(ChessBoard, ParseSanAcceptsCoordinateNotation) {
  Board board;
  Move m;
  ASSERT_TRUE(board.parseSan("e2e4", m));
  EXPECT_EQ(m.from, squareOf(4, 1));
  EXPECT_EQ(m.to, squareOf(4, 3));
  EXPECT_TRUE(m.flags & FLAG_DOUBLE_PUSH);

  Board promo;
  ASSERT_TRUE(promo.fromFen("8/4P3/8/8/8/8/8/K6k w - - 0 1"));
  ASSERT_TRUE(promo.parseSan("e7e8n", m));
  EXPECT_EQ(m.promo, KNIGHT);
}

TEST(ChessSearch, FindsMateInOne) {
  Board board;
  ASSERT_TRUE(board.fromFen("6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1"));
  Search search;
  const Move best = search.findBestMove(board, /*levelIndex=*/2, noHooks());
  char san[12];
  ASSERT_TRUE(board.toSan(best, san, sizeof(san)));
  EXPECT_STREQ(san, "Ra8#");
}

TEST(ChessSearch, TakesTheFreeQueen) {
  Board board;
  ASSERT_TRUE(board.fromFen("4k3/8/8/3q4/4B3/8/8/4K3 w - - 0 1"));
  Search search;
  const Move best = search.findBestMove(board, /*levelIndex=*/2, noHooks());
  EXPECT_EQ(best.to, squareOf(3, 4));
  EXPECT_TRUE(best.flags & FLAG_CAPTURE);
}

TEST(ChessSearch, ReturnsNullMoveWhenMated) {
  Board board;
  ASSERT_TRUE(board.fromFen("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3"));
  Search search;
  EXPECT_TRUE(search.findBestMove(board, 2, noHooks()).isNull());
}

TEST(ChessGame, ScholarsMateEndsTheGame) {
  Game game;
  const char* line[] = {"e4", "e5", "Bc4", "Nc6", "Qh5", "Nf6", "Qxf7#"};
  for (const char* san : line) ASSERT_TRUE(game.playSan(san)) << san;
  EXPECT_EQ(game.status(), Game::Status::Checkmate);
  EXPECT_EQ(game.plyCount(), 7);
  EXPECT_STREQ(game.sanAt(6), "Qxf7#");

  int count = 0;
  const Piece* takenByWhite = game.capturedBy(/*white=*/true, count);
  ASSERT_EQ(count, 1);
  EXPECT_EQ(typeOf(takenByWhite[0]), PAWN);
  EXPECT_EQ(game.materialLead(true), 1);
  EXPECT_EQ(game.materialLead(false), -1);
}

TEST(ChessGame, UndoRestoresPositionAndCaptures) {
  Game game;
  ASSERT_TRUE(game.playSan("e4"));
  ASSERT_TRUE(game.playSan("d5"));
  ASSERT_TRUE(game.playSan("exd5"));

  int count = 0;
  game.capturedBy(true, count);
  EXPECT_EQ(count, 1);

  char before[100];
  Game reference;
  ASSERT_TRUE(reference.playSan("e4"));
  ASSERT_TRUE(reference.playSan("d5"));
  ASSERT_TRUE(reference.board().toFen(before, sizeof(before)));

  ASSERT_TRUE(game.undoPly());
  char after[100];
  ASSERT_TRUE(game.board().toFen(after, sizeof(after)));
  EXPECT_STREQ(after, before);
  game.capturedBy(true, count);
  EXPECT_EQ(count, 0);
}

TEST(ChessGame, SaveStringRoundTripsPositionAndHistory) {
  Game game;
  const char* line[] = {"e4", "e5", "Nf3", "Nc6", "Bb5", "a6"};
  for (const char* san : line) ASSERT_TRUE(game.playSan(san));

  char saved[600];
  ASSERT_TRUE(game.toSaveString(saved, sizeof(saved)));

  Game restored;
  ASSERT_TRUE(restored.fromSaveString(saved));
  EXPECT_EQ(restored.plyCount(), game.plyCount());
  EXPECT_STREQ(restored.sanAt(4), "Bb5");

  char a[100], b[100];
  ASSERT_TRUE(game.board().toFen(a, sizeof(a)));
  ASSERT_TRUE(restored.board().toFen(b, sizeof(b)));
  EXPECT_STREQ(a, b);

  // A restored game is still undoable, which is why history is saved at all.
  EXPECT_TRUE(restored.undoPly());
  EXPECT_EQ(restored.plyCount(), 5);
}

TEST(ChessGame, DetectsStalemateAndInsufficientMaterial) {
  Game stalemate;
  ASSERT_TRUE(stalemate.board().fromFen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"));
  EXPECT_EQ(stalemate.status(), Game::Status::Stalemate);

  Game bareKings;
  ASSERT_TRUE(bareKings.board().fromFen("4k3/8/8/8/8/8/8/4KB2 w - - 0 1"));
  EXPECT_EQ(bareKings.status(), Game::Status::DrawMaterial);
}

TEST(ChessGame, CastlingAndEnPassantPlayThroughSan) {
  Game game;
  const char* line[] = {"e4", "e5", "Nf3", "Nf6", "Bc4", "Bc5", "O-O", "O-O"};
  for (const char* san : line) ASSERT_TRUE(game.playSan(san)) << san;
  EXPECT_EQ(typeOf(game.board().at(squareOf(6, 0))), KING);
  EXPECT_EQ(typeOf(game.board().at(squareOf(5, 0))), ROOK);

  Game ep;
  ASSERT_TRUE(ep.board().fromFen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2"));
  ASSERT_TRUE(ep.playSan("exd6"));
  EXPECT_EQ(ep.board().at(squareOf(3, 4)), NO_PIECE);
  EXPECT_EQ(typeOf(ep.board().at(squareOf(3, 5))), PAWN);
}

TEST(ChessOpeningBook, EveryBuiltInLineReplays) {
  // The book is hand-authored SAN. If a single move is mistyped the trainer
  // silently stops mid-line on the device, so replay all of them here.
  for (int i = 0; i < chess_book::OPENING_MOVES_COUNT; i++) {
    const auto& line = chess_book::OPENING_MOVES[i];
    Game game;
    int ply = 0;
    const char* p = line.moves;
    while (*p != '\0') {
      while (*p == ' ') p++;
      if (*p == '\0') break;
      char san[12];
      size_t n = 0;
      while (*p != '\0' && *p != ' ' && n + 1 < sizeof(san)) san[n++] = *p++;
      san[n] = '\0';
      ply++;
      ASSERT_TRUE(game.playSan(san)) << line.eco << " ply " << ply << ": " << san;
    }
    EXPECT_GE(ply, 4) << line.eco << " is too short to be a useful line";
    EXPECT_EQ(game.status(), Game::Status::Playing) << line.eco << " ends in a terminal position";
  }
}
