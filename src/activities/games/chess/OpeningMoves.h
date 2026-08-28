#pragma once

// Move data for the built-in openings, split out of OpeningBook.cpp so the
// host test suite can replay every line without pulling in I18n.
// Generated alongside OpeningBook.cpp; keep the two in step.

namespace chess_book {

struct LineMoves {
  const char* eco;
  const char* moves;  // space-separated SAN
};

constexpr LineMoves OPENING_MOVES[] = {
    {"C50", "e4 e5 Nf3 Nc6 Bc4 Bc5 c3 Nf6 d3 d6"},     {"C70", "e4 e5 Nf3 Nc6 Bb5 a6 Ba4 Nf6 O-O Be7"},
    {"C45", "e4 e5 Nf3 Nc6 d4 exd4 Nxd4 Bc5 Be3 Qf6"}, {"B90", "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 a6"},
    {"B70", "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 g6"},   {"B22", "e4 c5 c3 d5 exd5 Qxd5 d4 Nc6 Nf3 Bg4"},
    {"C02", "e4 e6 d4 d5 e5 c5 c3 Nc6 Nf3 Qb6"},       {"B10", "e4 c6 d4 d5 Nc3 dxe4 Nxe4 Bf5 Ng3 Bg6"},
    {"D30", "d4 d5 c4 e6 Nc3 Nf6 Bg5 Be7 e3 O-O"},     {"D20", "d4 d5 c4 dxc4 e4 Nf6 Nc3 e5 Nf3 exd4"},
    {"D10", "d4 d5 c4 c6 Nf3 Nf6 Nc3 dxc4 a4 Bf5"},    {"E60", "d4 Nf6 c4 g6 Nc3 Bg7 e4 d6 Nf3 O-O"},
    {"E20", "d4 Nf6 c4 e6 Nc3 Bb4 e3 O-O Bd3 d5"},     {"D02", "d4 d5 Bf4 Nf6 e3 e6 Nf3 c5 c3 Nc6"},
    {"A10", "c4 e5 Nc3 Nf6 g3 d5 cxd5 Nxd5 Bg2 Nb6"},  {"B01", "e4 d5 exd5 Qxd5 Nc3 Qa5 d4 Nf6 Nf3 c6"},
    {"B07", "e4 d6 d4 Nf6 Nc3 g6 Nf3 Bg7 Be2 O-O"},    {"C25", "e4 e5 Nc3 Nf6 f4 d5 fxe5 Nxe4 Nf3 Be7"},
    {"C33", "e4 e5 f4 exf4 Nf3 g5 Bc4 Bg7 O-O d6"},
};

constexpr int OPENING_MOVES_COUNT = static_cast<int>(sizeof(OPENING_MOVES) / sizeof(OPENING_MOVES[0]));

}  // namespace chess_book
