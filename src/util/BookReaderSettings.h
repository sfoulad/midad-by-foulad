#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"

// Per-book reading-settings sidecar: <bookCacheDir>/book_settings.bin.
//
// Holds only the per-book OVERRIDES (see the per-book block in
// CrossPointSettings.h) -- a book with no overrides has no sidecar at all.
// EpubReaderActivity applies the sidecar into SETTINGS.book* on open and
// clears the fields on exit, so the global settings file never sees them.
// A missing/corrupt sidecar simply reads as "no overrides".
namespace BookReaderSettings {

constexpr char FILE_NAME[] = "/book_settings.bin";
// Layout v4: 'B' 'K' version, then fontSize, arabicFontSize, lineSpacing,
// paragraphAlignment, arabicFontFamily, fontFamily (0xFF = inherit), then the
// two 32-byte family names ("" = inherit, "\x01" = force built-in -- same
// encoding as the fields). The byte LAYOUT is unchanged from v3 (still 73
// bytes) -- only buf[3]'s MEANING changed: v4 stores the LATIN reading font's
// actual point size (mirrors CrossPointSettings::fontPointSize), where v3 and
// earlier stored the old SMALL/MEDIUM/LARGE/EXTRA_LARGE slot (0..3). A v1-v3
// sidecar's buf[3] is folded the same way CrossPointSettings::fromJson() folds
// the legacy global value (12 + slot * 2) so an old per-book override doesn't
// silently become a nonsensically tiny point size; a v4 sidecar's buf[3] is
// read straight through with no enum-count bounds check (any value 0-254 is a
// plausible point size candidate -- snapToNearestPointSize() at render/load
// time is what actually keeps an unavailable value safe). buf[4] (arabicFontSize)
// is untouched by this -- Arabic sizing stays the fixed enum in every version.
// v2 lacked the fontFamily byte (there was only one built-in Latin family, so
// "force built-in" alone was unambiguous -- once Bitter/Lexend Deca added a
// second one, WHICH built-in needed its own byte, same reasoning as
// arabicFontFamily in v2). v1 lacked arabicFontFamily too. v1, v2, and v3 are
// all still readable.
constexpr uint8_t FORMAT_VERSION = 4;
constexpr size_t PAYLOAD_SIZE = 3 + 6 + 32 + 32;
constexpr size_t V1_PAYLOAD_SIZE = 3 + 4 + 32 + 32;
constexpr size_t V2_PAYLOAD_SIZE = 3 + 5 + 32 + 32;
// v3's payload is the same size as v4's (only buf[3]'s meaning changed between
// them) -- kept as a separate name purely for readability at the version-check
// site below, not because the byte count differs.
constexpr size_t V3_PAYLOAD_SIZE = PAYLOAD_SIZE;

inline uint8_t sanitizeEnum(const uint8_t v, const uint8_t count) {
  return v < count ? v : CrossPointSettings::BOOK_NO_OVERRIDE;
}

// Load the book's sidecar into SETTINGS.book*. Always starts from a clean
// no-overrides state so a book without a sidecar can't inherit the previous
// book's overrides.
inline void applyToSettings(const std::string& bookCachePath) {
  SETTINGS.clearBookOverrides();

  HalFile f;
  if (!Storage.openFileForRead("BKS", bookCachePath + FILE_NAME, f)) return;
  uint8_t buf[PAYLOAD_SIZE];
  const int got = f.read(buf, PAYLOAD_SIZE);
  if (buf[0] != 'B' || buf[1] != 'K') return;
  const bool v1 = buf[2] == 1 && got == static_cast<int>(V1_PAYLOAD_SIZE);
  const bool v2 = buf[2] == 2 && got == static_cast<int>(V2_PAYLOAD_SIZE);
  const bool v3 = buf[2] == 3 && got == static_cast<int>(V3_PAYLOAD_SIZE);
  const bool v4 = buf[2] == FORMAT_VERSION && got == static_cast<int>(PAYLOAD_SIZE);
  if (!v1 && !v2 && !v3 && !v4) return;
  const size_t namesAt = (v3 || v4) ? 9 : (v2 ? 8 : 7);

  if (v4) {
    // v4: buf[3] is a raw point size (or BOOK_NO_OVERRIDE) already.
    SETTINGS.bookFontSize = buf[3];
  } else {
    // v1-v3: buf[3] holds the legacy SMALL/MEDIUM/LARGE/EXTRA_LARGE slot (0..3)
    // -- fold it the same way CrossPointSettings::fromJson() folds the legacy
    // global value, so an old per-book override doesn't silently become a
    // nonsensically tiny point size.
    const uint8_t legacySlot = sanitizeEnum(buf[3], CrossPointSettings::FONT_SIZE_COUNT);
    SETTINGS.bookFontSize = legacySlot == CrossPointSettings::BOOK_NO_OVERRIDE
                                ? CrossPointSettings::BOOK_NO_OVERRIDE
                                : static_cast<uint8_t>(12 + legacySlot * 2);
  }
  SETTINGS.bookArabicFontSize = sanitizeEnum(buf[4], CrossPointSettings::FONT_SIZE_COUNT);
  SETTINGS.bookLineSpacing = sanitizeEnum(buf[5], CrossPointSettings::LINE_COMPRESSION_COUNT);
  SETTINGS.bookParagraphAlignment = sanitizeEnum(buf[6], CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT);
  if (v2 || v3 || v4) {
    SETTINGS.bookArabicFontFamily = sanitizeEnum(buf[7], CrossPointSettings::ARABIC_FONT_FAMILY_COUNT);
  }
  if (v3 || v4) {
    SETTINGS.bookFontFamily = sanitizeEnum(buf[8], CrossPointSettings::FONT_FAMILY_COUNT);
  }
  memcpy(SETTINGS.bookSdFontFamilyName, buf + namesAt, sizeof(SETTINGS.bookSdFontFamilyName));
  SETTINGS.bookSdFontFamilyName[sizeof(SETTINGS.bookSdFontFamilyName) - 1] = '\0';
  memcpy(SETTINGS.bookSdArabicFontFamilyName, buf + namesAt + 32, sizeof(SETTINGS.bookSdArabicFontFamilyName));
  SETTINGS.bookSdArabicFontFamilyName[sizeof(SETTINGS.bookSdArabicFontFamilyName) - 1] = '\0';

  if (SETTINGS.hasBookOverrides()) {
    LOG_INF("BKS", "Applied per-book settings from %s%s", bookCachePath.c_str(), FILE_NAME);
  }
}

// Persist SETTINGS.book* as the book's sidecar; no overrides = no sidecar.
inline bool saveFromSettings(const std::string& bookCachePath) {
  const std::string path = bookCachePath + FILE_NAME;
  if (!SETTINGS.hasBookOverrides()) {
    Storage.remove(path.c_str());
    return true;
  }

  uint8_t buf[PAYLOAD_SIZE];
  buf[0] = 'B';
  buf[1] = 'K';
  buf[2] = FORMAT_VERSION;
  buf[3] = SETTINGS.bookFontSize;
  buf[4] = SETTINGS.bookArabicFontSize;
  buf[5] = SETTINGS.bookLineSpacing;
  buf[6] = SETTINGS.bookParagraphAlignment;
  buf[7] = SETTINGS.bookArabicFontFamily;
  buf[8] = SETTINGS.bookFontFamily;
  memcpy(buf + 9, SETTINGS.bookSdFontFamilyName, sizeof(SETTINGS.bookSdFontFamilyName));
  memcpy(buf + 9 + 32, SETTINGS.bookSdArabicFontFamilyName, sizeof(SETTINGS.bookSdArabicFontFamilyName));

  HalFile f;
  if (!Storage.openFileForWrite("BKS", path, f)) {
    LOG_ERR("BKS", "Could not open %s for write", path.c_str());
    return false;
  }
  return f.write(buf, PAYLOAD_SIZE) == PAYLOAD_SIZE;
}

}  // namespace BookReaderSettings
