#pragma once

#include <cstdint>

#include "I18nKeys.h"

// Gamified "reader title" tiers shown on the Dashboard screensaver, keyed off
// READING_STATS.getBooksFinishedCount() (user-specified table). Ascending by
// minBooks; kept as a plain array (13 rows) rather than a map -- a linear
// reverse-scan is plenty cheap at this size and avoids a heap-backed container
// for something this static.
struct ReaderTitleEntry {
  uint32_t minBooks;
  int level;
  StrId titleId;
};

constexpr ReaderTitleEntry kReaderTitles[] = {
    {0, 1, StrId::STR_READER_TITLE_1},      {1, 2, StrId::STR_READER_TITLE_2},
    {10, 3, StrId::STR_READER_TITLE_3},     {20, 4, StrId::STR_READER_TITLE_4},
    {50, 5, StrId::STR_READER_TITLE_5},     {70, 6, StrId::STR_READER_TITLE_6},
    {100, 7, StrId::STR_READER_TITLE_7},    {150, 8, StrId::STR_READER_TITLE_8},
    {250, 9, StrId::STR_READER_TITLE_9},    {400, 10, StrId::STR_READER_TITLE_10},
    {600, 11, StrId::STR_READER_TITLE_11},  {800, 12, StrId::STR_READER_TITLE_12},
    {1000, 13, StrId::STR_READER_TITLE_13},
};
constexpr int kReaderTitleCount = sizeof(kReaderTitles) / sizeof(kReaderTitles[0]);

// Last entry whose minBooks <= booksFinished (table is ascending, so a reverse
// scan finds it immediately without needing std::upper_bound/<algorithm>).
inline const ReaderTitleEntry& getReaderTitleForBooksFinished(const uint32_t booksFinished) {
  for (int i = kReaderTitleCount - 1; i >= 0; --i) {
    if (booksFinished >= kReaderTitles[i].minBooks) return kReaderTitles[i];
  }
  return kReaderTitles[0];
}
