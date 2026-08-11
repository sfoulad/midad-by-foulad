#pragma once

#include <cstddef>
#include <cstdint>

// The Holy Quran, shipped inside the firmware image (data/quran.epub, embedded
// into flash via board_build.embed_files; built by tools/quran/build_quran_epub.py
// from the source EPUB -- ornate ayah markers, Arabic-Indic surah numbers).
//
// Enabling the Settings -> Apps toggle (MidadAppSettings::quranEnabled) extracts
// the EPUB from flash to the SD card at PATH; My Books then pins it as the first
// book (see RecentBooksActivity). No network involved: the whole book lives in
// the firmware, satisfying "works out of the box, offline".
namespace QuranBook {

constexpr char DIR[] = "/Quran";
constexpr char PATH[] = "/Quran/Quran.epub";
constexpr char TITLE[] = "القرآن الكريم";

// Embedded EPUB bytes (flash). size 0 if unavailable (simulator without the file).
const uint8_t* data(size_t& size);

// True when the toggle is on AND the extracted file is present on SD.
bool isPinned();

// Extract the embedded EPUB to PATH if missing or size-mismatched (a torn
// previous extraction). Returns true when the file is in place afterwards.
bool ensureExtracted();

}  // namespace QuranBook
