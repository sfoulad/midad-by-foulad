#include "QuranBook.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "util/BookReaderSettings.h"

namespace {

// Same cache-dir formula as the Epub constructor (path-hash keyed).
std::string quranCachePath() {
  return "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(std::string(QuranBook::PATH)));
}

// Give the Quran its reading face: a per-book sidecar selecting the built-in
// Amiri family (revival of the 1924 Cairo mushaf typeface -- renders full
// harakat with our static pipeline; Neirizi is also in the drawer's Font Name
// picker). Written only when no sidecar exists yet, so any choice the user
// later makes in the reader drawer sticks across re-extractions and firmware
// updates.
void writeDefaultSidecarIfMissing() {
  const std::string cacheDir = quranCachePath();
  const std::string sidecar = cacheDir + BookReaderSettings::FILE_NAME;
  if (Storage.exists(sidecar.c_str())) return;
  Storage.mkdir(cacheDir.c_str());

  uint8_t buf[BookReaderSettings::PAYLOAD_SIZE];
  memset(buf, 0, sizeof(buf));
  buf[0] = 'B';
  buf[1] = 'K';
  buf[2] = BookReaderSettings::FORMAT_VERSION;
  buf[3] = CrossPointSettings::BOOK_NO_OVERRIDE;  // fontSize: inherit
  buf[4] = CrossPointSettings::BOOK_NO_OVERRIDE;  // arabicFontSize: inherit
  buf[5] = CrossPointSettings::BOOK_NO_OVERRIDE;  // lineSpacing: inherit
  buf[6] = CrossPointSettings::JUSTIFIED;         // mushaf convention: justified lines
  buf[7] = CrossPointSettings::AMIRI;             // built-in Arabic family: Amiri
  // Arabic family-source: force the built-in path (buf[8..39] Latin stays "").
  buf[8 + 32] = CrossPointSettings::BOOK_FORCE_BUILTIN_FAMILY[0];

  HalFile f;
  if (Storage.openFileForWrite("QURAN", sidecar, f)) {
    f.write(buf, sizeof(buf));
    LOG_INF("QURAN", "Default Quran font sidecar written (Amiri)");
  }
}

}  // namespace

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
      writeDefaultSidecarIfMissing();
      return true;
    }
  }

  // The embedded text changed (firmware update): the parsed cache under the
  // Quran's cache dir describes the OLD content -- clear it or the reader
  // keeps showing stale pages. The user's font/size sidecar survives.
  {
    const std::string cacheDir = quranCachePath();
    const std::string sidecar = cacheDir + BookReaderSettings::FILE_NAME;
    std::vector<uint8_t> savedSidecar;
    HalFile sf;
    if (Storage.openFileForRead("QURAN", sidecar, sf)) {
      savedSidecar.resize(sf.size());
      if (sf.read(savedSidecar.data(), savedSidecar.size()) != static_cast<int>(savedSidecar.size())) {
        savedSidecar.clear();
      }
    }
    Storage.removeDir(cacheDir.c_str());
    if (!savedSidecar.empty()) {
      Storage.mkdir(cacheDir.c_str());
      HalFile rf;
      if (Storage.openFileForWrite("QURAN", sidecar, rf)) {
        rf.write(savedSidecar.data(), savedSidecar.size());
      }
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
  writeDefaultSidecarIfMissing();
  return true;
}

}  // namespace QuranBook
