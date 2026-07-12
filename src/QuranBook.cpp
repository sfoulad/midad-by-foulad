#include "QuranBook.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"

#ifndef SIMULATOR
// Embedded by board_build.embed_files = data/quran.epub (see platformio.ini).
extern const uint8_t _binary_data_quran_epub_start[] asm("_binary_data_quran_epub_start");
extern const uint8_t _binary_data_quran_epub_end[] asm("_binary_data_quran_epub_end");
#endif

namespace QuranBook {

const uint8_t* data(size_t& size) {
#ifdef SIMULATOR
  // The native simulator build has no flash embedding; read the repo asset
  // from the host filesystem (the simulator runs from the repo root).
  static std::vector<uint8_t> hostData;
  if (hostData.empty()) {
    FILE* f = fopen("data/quran.epub", "rb");
    if (!f) {
      size = 0;
      return nullptr;
    }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    hostData.resize(len);
    if (fread(hostData.data(), 1, len, f) != static_cast<size_t>(len)) {
      hostData.clear();
    }
    fclose(f);
  }
  size = hostData.size();
  return hostData.empty() ? nullptr : hostData.data();
#else
  size = static_cast<size_t>(_binary_data_quran_epub_end - _binary_data_quran_epub_start);
  return _binary_data_quran_epub_start;
#endif
}

bool isPinned() { return SETTINGS.quranEnabled != 0 && Storage.exists(PATH); }

bool ensureExtracted() {
  size_t embeddedSize = 0;
  const uint8_t* embedded = data(embeddedSize);
  if (!embedded || embeddedSize == 0) {
    LOG_ERR("QURAN", "Embedded Quran data unavailable");
    return false;
  }

  // Already extracted and whole (a size mismatch means a torn previous write
  // or an older firmware's copy -- re-extract to match this build).
  if (Storage.exists(PATH)) {
    HalFile existing;
    if (Storage.openFileForRead("QURAN", PATH, existing) && existing.size() == embeddedSize) {
      return true;
    }
  }

  Storage.mkdir(DIR);
  HalFile out;
  if (!Storage.openFileForWrite("QURAN", PATH, out)) {
    LOG_ERR("QURAN", "Cannot open %s for write", PATH);
    return false;
  }
  // Chunked copy: flash-resident source, bounded RAM, SD-friendly block size.
  constexpr size_t CHUNK = 4096;
  size_t written = 0;
  while (written < embeddedSize) {
    const size_t n = std::min(CHUNK, embeddedSize - written);
    if (out.write(embedded + written, n) != n) {
      LOG_ERR("QURAN", "Short write extracting Quran at %u/%u", (unsigned)written, (unsigned)embeddedSize);
      return false;
    }
    written += n;
  }
  out.flush();
  LOG_INF("QURAN", "Extracted %s (%u bytes)", PATH, (unsigned)embeddedSize);
  return true;
}

}  // namespace QuranBook
