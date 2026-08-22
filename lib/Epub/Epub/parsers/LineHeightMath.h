#pragma once

#include <algorithm>

// Pure arithmetic shared by ChapterHtmlSlimParser::computeLineHeight() (a single line's
// page-fit height) and finishTableRow() (the max row height across a table row's columns).
// Kept free of the GfxRenderer/TextBlock/ScriptDetector stack that supplies these inputs so
// both call sites use one formula and test/line_height_math/ can verify it on a host build.
inline int effectiveLineHeight(const int latinLineHeight, const int arabicLineHeight, const bool hasArabic,
                               const float lineCompression) {
  const int base = hasArabic ? std::max(latinLineHeight, arabicLineHeight) : latinLineHeight;
  return static_cast<int>(base * lineCompression);
}
