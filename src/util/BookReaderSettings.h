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
// Layout v3: 'B' 'K' version, then fontSize, arabicFontSize, lineSpacing,
// paragraphAlignment, arabicFontFamily, fontFamily (0xFF = inherit), then the
// two 32-byte family names ("" = inherit, "\x01" = force built-in -- same
// encoding as the fields). v2 lacked the fontFamily byte (there was only one
// built-in Latin family, so "force built-in" alone was unambiguous -- once
// Bitter/Lexend Deca added a second one, WHICH built-in needed its own byte,
// same reasoning as arabicFontFamily in v2). v1 lacked arabicFontFamily too.
// Both v1 and v2 are still readable.
constexpr uint8_t FORMAT_VERSION = 3;
constexpr size_t PAYLOAD_SIZE = 3 + 6 + 32 + 32;
constexpr size_t V1_PAYLOAD_SIZE = 3 + 4 + 32 + 32;
constexpr size_t V2_PAYLOAD_SIZE = 3 + 5 + 32 + 32;

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
  const bool v3 = buf[2] == FORMAT_VERSION && got == static_cast<int>(PAYLOAD_SIZE);
  if (!v1 && !v2 && !v3) return;
  const size_t namesAt = v3 ? 9 : (v2 ? 8 : 7);

  SETTINGS.bookFontSize = sanitizeEnum(buf[3], CrossPointSettings::FONT_SIZE_COUNT);
  SETTINGS.bookArabicFontSize = sanitizeEnum(buf[4], CrossPointSettings::FONT_SIZE_COUNT);
  SETTINGS.bookLineSpacing = sanitizeEnum(buf[5], CrossPointSettings::LINE_COMPRESSION_COUNT);
  SETTINGS.bookParagraphAlignment = sanitizeEnum(buf[6], CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT);
  if (v2 || v3) {
    SETTINGS.bookArabicFontFamily = sanitizeEnum(buf[7], CrossPointSettings::ARABIC_FONT_FAMILY_COUNT);
  }
  if (v3) {
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
