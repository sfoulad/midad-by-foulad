#pragma once

#include <SdCardFontRegistry.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Point sizes the built-in LATIN reading font family ships (see fontIds.h:
// LEXENDDECA_12/14/16/18_FONT_ID). Used both as the selectable set when no SD
// font family is active, and as the snap target when one is.
inline constexpr uint8_t BUILTIN_READER_POINT_SIZES[] = {12, 14, 16, 18};

// Selectable point sizes for the current family: an SD family's actually-
// installed sizes when one is active and known to `registry`, otherwise
// BUILTIN_READER_POINT_SIZES. Never empty.
std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName);

// Nearest value in `sizes` to `pt` (ties favor the earlier/smaller entry, same
// tie-break as SdCardFontFamilyInfo::findNearestSize()). Returns `pt` unchanged
// if `sizes` is empty/null -- callers that always pass BUILTIN_READER_POINT_SIZES
// or a non-empty readerFontPointSizes() result never hit that case.
uint8_t snapToNearestPointSize(const uint8_t* sizes, size_t count, uint8_t pt);
inline uint8_t snapToNearestPointSize(const std::vector<uint8_t>& sizes, const uint8_t pt) {
  return sizes.empty() ? pt : snapToNearestPointSize(sizes.data(), sizes.size(), pt);
}
