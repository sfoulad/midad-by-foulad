#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace BidiUtils {

// Paragraph-level P2/P3: scan the first N strong chars per word to find base direction.
inline constexpr int RTL_PARAGRAPH_PROBE_DEPTH = 5;

bool startsWithRtl(const char* utf8, int maxStrongChars = RTL_PARAGRAPH_PROBE_DEPTH);

// Per UAX#9 rule P2: find the FIRST character in utf8 with a strong direction (L, R, or
// AL), skipping any number of leading weak/neutral characters (digits, punctuation, an
// opening quote). On finding one, sets isRtl and returns true. Returns false (isRtl
// untouched) if no strong character appears within maxStrongChars codepoints, so the
// caller can continue the scan into the next word/token instead of guessing a direction
// from inconclusive input.
bool firstStrongDirection(const char* utf8, bool& isRtl, int maxStrongChars = RTL_PARAGRAPH_PROBE_DEPTH);

int detectParagraphLevel(const char* utf8, int fallbackLevel = 0, int maxStrongChars = 64);

// paragraphLevel: -1 = auto-detect, 0 = LTR, 1 = RTL
bool applyBidiVisual(const char* utf8, std::string& out, int paragraphLevel = -1);

bool computeVisualWordOrder(const std::vector<std::string>& words, bool paragraphIsRtl,
                            std::vector<uint16_t>& visualOrder);

}  // namespace BidiUtils
