#include "OpeningBook.h"

#include <cstdio>
#include <cstring>

#include "OpeningMoves.h"

namespace chess_book {
namespace {

constexpr Tip TIPS_ITALIAN[] = {
    {1, StrId::STR_CHESS_TIP_ITALIAN_1, StrId::STR_CHESS_TIPTEXT_ITALIAN_1},
    {3, StrId::STR_CHESS_TIP_ITALIAN_2, StrId::STR_CHESS_TIPTEXT_ITALIAN_2},
    {5, StrId::STR_CHESS_TIP_ITALIAN_3, StrId::STR_CHESS_TIPTEXT_ITALIAN_3},
    {6, StrId::STR_CHESS_TIP_ITALIAN_4, StrId::STR_CHESS_TIPTEXT_ITALIAN_4},
    {7, StrId::STR_CHESS_TIP_ITALIAN_5, StrId::STR_CHESS_TIPTEXT_ITALIAN_5},
    {9, StrId::STR_CHESS_TIP_ITALIAN_6, StrId::STR_CHESS_TIPTEXT_ITALIAN_6},
};

constexpr Tip TIPS_RUY_LOPEZ[] = {
    {5, StrId::STR_CHESS_TIP_RUY_LOPEZ_1, StrId::STR_CHESS_TIPTEXT_RUY_LOPEZ_1},
    {6, StrId::STR_CHESS_TIP_RUY_LOPEZ_2, StrId::STR_CHESS_TIPTEXT_RUY_LOPEZ_2},
    {7, StrId::STR_CHESS_TIP_RUY_LOPEZ_3, StrId::STR_CHESS_TIPTEXT_RUY_LOPEZ_3},
    {9, StrId::STR_CHESS_TIP_RUY_LOPEZ_4, StrId::STR_CHESS_TIPTEXT_RUY_LOPEZ_4},
};

constexpr Tip TIPS_SCOTCH[] = {
    {5, StrId::STR_CHESS_TIP_SCOTCH_1, StrId::STR_CHESS_TIPTEXT_SCOTCH_1},
    {7, StrId::STR_CHESS_TIP_SCOTCH_2, StrId::STR_CHESS_TIPTEXT_SCOTCH_2},
    {8, StrId::STR_CHESS_TIP_SCOTCH_3, StrId::STR_CHESS_TIPTEXT_SCOTCH_3},
};

constexpr Tip TIPS_SICILIAN_NAJDORF[] = {
    {2, StrId::STR_CHESS_TIP_SICILIAN_NAJDORF_1, StrId::STR_CHESS_TIPTEXT_SICILIAN_NAJDORF_1},
    {5, StrId::STR_CHESS_TIP_SICILIAN_NAJDORF_2, StrId::STR_CHESS_TIPTEXT_SICILIAN_NAJDORF_2},
    {10, StrId::STR_CHESS_TIP_SICILIAN_NAJDORF_3, StrId::STR_CHESS_TIPTEXT_SICILIAN_NAJDORF_3},
};

constexpr Tip TIPS_SICILIAN_DRAGON[] = {
    {10, StrId::STR_CHESS_TIP_SICILIAN_DRAGON_1, StrId::STR_CHESS_TIPTEXT_SICILIAN_DRAGON_1},
    {5, StrId::STR_CHESS_TIP_SICILIAN_DRAGON_2, StrId::STR_CHESS_TIPTEXT_SICILIAN_DRAGON_2},
};

constexpr Tip TIPS_SICILIAN_ALAPIN[] = {
    {3, StrId::STR_CHESS_TIP_SICILIAN_ALAPIN_1, StrId::STR_CHESS_TIPTEXT_SICILIAN_ALAPIN_1},
    {4, StrId::STR_CHESS_TIP_SICILIAN_ALAPIN_2, StrId::STR_CHESS_TIPTEXT_SICILIAN_ALAPIN_2},
    {7, StrId::STR_CHESS_TIP_SICILIAN_ALAPIN_3, StrId::STR_CHESS_TIPTEXT_SICILIAN_ALAPIN_3},
};

constexpr Tip TIPS_FRENCH_ADVANCE[] = {
    {2, StrId::STR_CHESS_TIP_FRENCH_ADVANCE_1, StrId::STR_CHESS_TIPTEXT_FRENCH_ADVANCE_1},
    {5, StrId::STR_CHESS_TIP_FRENCH_ADVANCE_2, StrId::STR_CHESS_TIPTEXT_FRENCH_ADVANCE_2},
    {6, StrId::STR_CHESS_TIP_FRENCH_ADVANCE_3, StrId::STR_CHESS_TIPTEXT_FRENCH_ADVANCE_3},
    {10, StrId::STR_CHESS_TIP_FRENCH_ADVANCE_4, StrId::STR_CHESS_TIPTEXT_FRENCH_ADVANCE_4},
};

constexpr Tip TIPS_CARO_KANN[] = {
    {2, StrId::STR_CHESS_TIP_CARO_KANN_1, StrId::STR_CHESS_TIPTEXT_CARO_KANN_1},
    {8, StrId::STR_CHESS_TIP_CARO_KANN_2, StrId::STR_CHESS_TIPTEXT_CARO_KANN_2},
    {9, StrId::STR_CHESS_TIP_CARO_KANN_3, StrId::STR_CHESS_TIPTEXT_CARO_KANN_3},
};

constexpr Tip TIPS_QGD[] = {
    {3, StrId::STR_CHESS_TIP_QGD_1, StrId::STR_CHESS_TIPTEXT_QGD_1},
    {4, StrId::STR_CHESS_TIP_QGD_2, StrId::STR_CHESS_TIPTEXT_QGD_2},
    {7, StrId::STR_CHESS_TIP_QGD_3, StrId::STR_CHESS_TIPTEXT_QGD_3},
};

constexpr Tip TIPS_QGA[] = {
    {4, StrId::STR_CHESS_TIP_QGA_1, StrId::STR_CHESS_TIPTEXT_QGA_1},
    {5, StrId::STR_CHESS_TIP_QGA_2, StrId::STR_CHESS_TIPTEXT_QGA_2},
};

constexpr Tip TIPS_SLAV[] = {
    {4, StrId::STR_CHESS_TIP_SLAV_1, StrId::STR_CHESS_TIPTEXT_SLAV_1},
    {8, StrId::STR_CHESS_TIP_SLAV_2, StrId::STR_CHESS_TIPTEXT_SLAV_2},
    {9, StrId::STR_CHESS_TIP_SLAV_3, StrId::STR_CHESS_TIPTEXT_SLAV_3},
};

constexpr Tip TIPS_KINGS_INDIAN[] = {
    {2, StrId::STR_CHESS_TIP_KINGS_INDIAN_1, StrId::STR_CHESS_TIPTEXT_KINGS_INDIAN_1},
    {6, StrId::STR_CHESS_TIP_KINGS_INDIAN_2, StrId::STR_CHESS_TIPTEXT_KINGS_INDIAN_2},
    {10, StrId::STR_CHESS_TIP_KINGS_INDIAN_3, StrId::STR_CHESS_TIPTEXT_KINGS_INDIAN_3},
};

constexpr Tip TIPS_NIMZO_INDIAN[] = {
    {6, StrId::STR_CHESS_TIP_NIMZO_INDIAN_1, StrId::STR_CHESS_TIPTEXT_NIMZO_INDIAN_1},
    {7, StrId::STR_CHESS_TIP_NIMZO_INDIAN_2, StrId::STR_CHESS_TIPTEXT_NIMZO_INDIAN_2},
    {10, StrId::STR_CHESS_TIP_NIMZO_INDIAN_3, StrId::STR_CHESS_TIPTEXT_NIMZO_INDIAN_3},
};

constexpr Tip TIPS_LONDON[] = {
    {3, StrId::STR_CHESS_TIP_LONDON_1, StrId::STR_CHESS_TIPTEXT_LONDON_1},
    {9, StrId::STR_CHESS_TIP_LONDON_2, StrId::STR_CHESS_TIPTEXT_LONDON_2},
};

constexpr Tip TIPS_ENGLISH[] = {
    {1, StrId::STR_CHESS_TIP_ENGLISH_1, StrId::STR_CHESS_TIPTEXT_ENGLISH_1},
    {5, StrId::STR_CHESS_TIP_ENGLISH_2, StrId::STR_CHESS_TIPTEXT_ENGLISH_2},
};

constexpr Tip TIPS_SCANDINAVIAN[] = {
    {2, StrId::STR_CHESS_TIP_SCANDINAVIAN_1, StrId::STR_CHESS_TIPTEXT_SCANDINAVIAN_1},
    {4, StrId::STR_CHESS_TIP_SCANDINAVIAN_2, StrId::STR_CHESS_TIPTEXT_SCANDINAVIAN_2},
    {10, StrId::STR_CHESS_TIP_SCANDINAVIAN_3, StrId::STR_CHESS_TIPTEXT_SCANDINAVIAN_3},
};

constexpr Tip TIPS_PIRC[] = {
    {2, StrId::STR_CHESS_TIP_PIRC_1, StrId::STR_CHESS_TIPTEXT_PIRC_1},
    {6, StrId::STR_CHESS_TIP_PIRC_2, StrId::STR_CHESS_TIPTEXT_PIRC_2},
};

constexpr Tip TIPS_VIENNA[] = {
    {3, StrId::STR_CHESS_TIP_VIENNA_1, StrId::STR_CHESS_TIPTEXT_VIENNA_1},
    {6, StrId::STR_CHESS_TIP_VIENNA_2, StrId::STR_CHESS_TIPTEXT_VIENNA_2},
};

constexpr Tip TIPS_KINGS_GAMBIT[] = {
    {3, StrId::STR_CHESS_TIP_KINGS_GAMBIT_1, StrId::STR_CHESS_TIPTEXT_KINGS_GAMBIT_1},
    {5, StrId::STR_CHESS_TIP_KINGS_GAMBIT_2, StrId::STR_CHESS_TIPTEXT_KINGS_GAMBIT_2},
    {9, StrId::STR_CHESS_TIP_KINGS_GAMBIT_3, StrId::STR_CHESS_TIPTEXT_KINGS_GAMBIT_3},
};

constexpr Line LINES[] = {
    {StrId::STR_CHESS_OP_ITALIAN, OPENING_MOVES[0].eco, OPENING_MOVES[0].moves, TIPS_ITALIAN, 6},
    {StrId::STR_CHESS_OP_RUY_LOPEZ, OPENING_MOVES[1].eco, OPENING_MOVES[1].moves, TIPS_RUY_LOPEZ, 4},
    {StrId::STR_CHESS_OP_SCOTCH, OPENING_MOVES[2].eco, OPENING_MOVES[2].moves, TIPS_SCOTCH, 3},
    {StrId::STR_CHESS_OP_SICILIAN_NAJDORF, OPENING_MOVES[3].eco, OPENING_MOVES[3].moves, TIPS_SICILIAN_NAJDORF, 3},
    {StrId::STR_CHESS_OP_SICILIAN_DRAGON, OPENING_MOVES[4].eco, OPENING_MOVES[4].moves, TIPS_SICILIAN_DRAGON, 2},
    {StrId::STR_CHESS_OP_SICILIAN_ALAPIN, OPENING_MOVES[5].eco, OPENING_MOVES[5].moves, TIPS_SICILIAN_ALAPIN, 3},
    {StrId::STR_CHESS_OP_FRENCH_ADVANCE, OPENING_MOVES[6].eco, OPENING_MOVES[6].moves, TIPS_FRENCH_ADVANCE, 4},
    {StrId::STR_CHESS_OP_CARO_KANN, OPENING_MOVES[7].eco, OPENING_MOVES[7].moves, TIPS_CARO_KANN, 3},
    {StrId::STR_CHESS_OP_QGD, OPENING_MOVES[8].eco, OPENING_MOVES[8].moves, TIPS_QGD, 3},
    {StrId::STR_CHESS_OP_QGA, OPENING_MOVES[9].eco, OPENING_MOVES[9].moves, TIPS_QGA, 2},
    {StrId::STR_CHESS_OP_SLAV, OPENING_MOVES[10].eco, OPENING_MOVES[10].moves, TIPS_SLAV, 3},
    {StrId::STR_CHESS_OP_KINGS_INDIAN, OPENING_MOVES[11].eco, OPENING_MOVES[11].moves, TIPS_KINGS_INDIAN, 3},
    {StrId::STR_CHESS_OP_NIMZO_INDIAN, OPENING_MOVES[12].eco, OPENING_MOVES[12].moves, TIPS_NIMZO_INDIAN, 3},
    {StrId::STR_CHESS_OP_LONDON, OPENING_MOVES[13].eco, OPENING_MOVES[13].moves, TIPS_LONDON, 2},
    {StrId::STR_CHESS_OP_ENGLISH, OPENING_MOVES[14].eco, OPENING_MOVES[14].moves, TIPS_ENGLISH, 2},
    {StrId::STR_CHESS_OP_SCANDINAVIAN, OPENING_MOVES[15].eco, OPENING_MOVES[15].moves, TIPS_SCANDINAVIAN, 3},
    {StrId::STR_CHESS_OP_PIRC, OPENING_MOVES[16].eco, OPENING_MOVES[16].moves, TIPS_PIRC, 2},
    {StrId::STR_CHESS_OP_VIENNA, OPENING_MOVES[17].eco, OPENING_MOVES[17].moves, TIPS_VIENNA, 2},
    {StrId::STR_CHESS_OP_KINGS_GAMBIT, OPENING_MOVES[18].eco, OPENING_MOVES[18].moves, TIPS_KINGS_GAMBIT, 3},
};

}  // namespace

int lineCount() { return static_cast<int>(sizeof(LINES) / sizeof(LINES[0])); }

const Line& lineAt(int index) {
  if (index < 0) index = 0;
  if (index >= lineCount()) index = lineCount() - 1;
  return LINES[index];
}

const Tip* tipForPly(const Line& line, int ply) {
  for (uint8_t i = 0; i < line.tipCount; i++) {
    if (line.tips[i].ply == ply) return &line.tips[i];
  }
  return nullptr;
}

void formatPreview(const Line& line, int maxPlies, char* out, size_t cap) {
  if (cap == 0) return;
  out[0] = '\0';
  size_t used = 0;
  int ply = 0;
  const char* p = line.moves;
  while (*p != '\0' && ply < maxPlies) {
    char token[10];
    size_t t = 0;
    while (*p != '\0' && *p != ' ' && t + 1 < sizeof(token)) token[t++] = *p++;
    token[t] = '\0';
    while (*p == ' ') p++;
    if (t == 0) break;

    // "1.e4 e5 2.Nf3 ..." -- the move number only precedes White's move.
    char chunk[16];
    const int written = (ply % 2 == 0)
                            ? snprintf(chunk, sizeof(chunk), "%s%d.%s", used == 0 ? "" : " ", ply / 2 + 1, token)
                            : snprintf(chunk, sizeof(chunk), " %s", token);
    if (written <= 0 || used + static_cast<size_t>(written) + 1 > cap) break;
    memcpy(out + used, chunk, static_cast<size_t>(written) + 1);
    used += static_cast<size_t>(written);
    ply++;
  }
}

}  // namespace chess_book
