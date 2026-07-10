#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct CodepointInfo {
  uint32_t value;
  size_t byteOffset;
};

uint32_t toLowerLatin(uint32_t cp);
uint32_t toLowerCyrillic(uint32_t cp);

bool isLatinLetter(uint32_t cp);
bool isCyrillicLetter(uint32_t cp);
// Arabic-script letters/marks across the base block, supplements, and the shaped
// presentation forms. Used to exclude Arabic words from fallback break-anywhere
// hyphenation: Arabic never hyphenates, and a mid-word break also destroys the
// cursive joining forms on both sides of the split.
bool isArabicLetter(uint32_t cp);

bool isAlphabetic(uint32_t cp);
// True if any codepoint in the word is Arabic script -- see isArabicLetter().
bool containsArabicLetter(const std::vector<CodepointInfo>& cps);
bool isPunctuation(uint32_t cp);
bool isAsciiDigit(uint32_t cp);
bool isApostrophe(uint32_t cp);
bool isExplicitHyphen(uint32_t cp);
bool isSoftHyphen(uint32_t cp);
void trimSurroundingPunctuationAndFootnote(std::vector<CodepointInfo>& cps);
std::vector<CodepointInfo> collectCodepoints(const std::string& word);
