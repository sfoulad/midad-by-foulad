#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>

// The built-in openings the trainer walks through. Everything lives in flash:
// the move list is a SAN string parsed by chess::Board, and the text is string
// ids, so a line costs a few dozen bytes of DRAM only while it is on screen.
namespace chess_book {

struct Tip {
  uint8_t ply;  // 1-based: 1 is after White's first move
  StrId title;  // short heading, e.g. "Claim the centre"
  StrId body;   // the explanation shown under the board
};

struct Line {
  StrId name;
  const char* eco;
  const char* moves;  // space-separated SAN
  const Tip* tips;
  uint8_t tipCount;
};

int lineCount();
const Line& lineAt(int index);

// The tip attached to `ply`, or nullptr when that move has none.
const Tip* tipForPly(const Line& line, int ply);

// First `maxPlies` moves formatted as "1.e4 e5 2.Nf3", for the list subtitle.
void formatPreview(const Line& line, int maxPlies, char* out, size_t cap);

}  // namespace chess_book
