#pragma once
#include <cstdint>

namespace ArabicShaper {

enum class JoiningType : uint8_t {
  NON_JOINING,    // Does not join (e.g. non-Arabic chars)
  RIGHT_JOINING,  // Joins only to the right (Alef, Dal, Thal, Ra, Zain, Waw, Teh Marbuta)
  DUAL_JOINING,   // Joins on both sides (most letters)
  TRANSPARENT     // Diacritics - don't affect joining
};

JoiningType getJoiningType(uint32_t cp);
bool isArabicBaseChar(uint32_t cp);
bool isArabicDiacritic(uint32_t cp);

// True for the subset of tashkeel/harakat (isArabicDiacritic) that render BELOW the
// base letter (kasra, kasratan, hamza-below and a few rare below-baseline Quranic marks).
// Everything else diacritic renders above (fatha, damma, sukun, shadda, tanwin fath/damm,
// superscript alef, etc.). Used to pick which side of the base a combining mark stacks on.
bool isArabicBelowMark(uint32_t cp);

}  // namespace ArabicShaper
