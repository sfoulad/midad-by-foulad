#include "OpdsCoverCache.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <OpdsParser.h>
#include <PngToBmpConverter.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "OpdsCoverCachePath.h"
#include "network/HttpDownloader.h"
#include "util/DebugLog.h"
#include "util/RollingSdLog.h"

namespace {
constexpr char TEMP_PATH[] = "/.crosspoint/opds_covers/.tmp_cover";

// Which decoder the downloaded bytes actually need.
//
// The feed's `type` attribute cannot answer this on its own: Midad Core's Library
// entries carry a cover link with NO type at all (a Library cover resolves in two
// tiers -- the reader's own image, then the Store book's -- and naming the media
// type would cost a query per entry, so Atom's optional `type` is simply omitted).
// An absent type read as "not PNG" sends every Library cover to the JPEG decoder,
// which fails on all of them. The bytes are authoritative and eight of them settle
// it, so they decide; the declared type is only the tie-breaker for anything that
// matches neither signature.
//
// Stack-only: an 8-byte buffer and one read, no allocation on a path that is
// already this close to its heap guard.
bool downloadedCoverIsPng(HalFile& file, const std::string& declaredType) {
  static constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  static constexpr uint8_t JPEG_SIGNATURE[3] = {0xFF, 0xD8, 0xFF};

  uint8_t header[sizeof(PNG_SIGNATURE)] = {};
  const int got = file.read(header, sizeof(header));
  // Rewind unconditionally: the decoders below read from position 0, and a failed
  // sniff must not leave the handle advanced.
  file.seek(0);

  if (got == static_cast<int>(sizeof(PNG_SIGNATURE)) && memcmp(header, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) == 0) {
    return true;
  }
  if (got >= static_cast<int>(sizeof(JPEG_SIGNATURE)) && memcmp(header, JPEG_SIGNATURE, sizeof(JPEG_SIGNATURE)) == 0) {
    return false;
  }
  return declaredType.find("png") != std::string::npos;
}

// Same thresholds as OpdsBookBrowserActivity.cpp's hasHeapForCoverWork() (that crash's
// own diagnosis: a blank-panic report from 1.8.40-rc traced to an ordinary std::string
// growth hitting the throwing global operator new under -fno-exceptions, which calls
// std::terminate() directly -- no abort() message, hence no panic reason recorded).
// That guard runs once per cover, before this whole function starts. It does not cover
// THIS function's own multi-step sequence: HttpDownloader::downloadToFile() (HTTP/TLS
// buffers) then the PNG/JPEG decoder (its own working buffers) each fragment the heap
// further, and every std::string built in between -- including getOpdsCoverCachePath()
// itself, called again below -- is exposed to the same failure mode regardless of how
// healthy the heap looked when the caller's pre-loop check ran. Re-checking here, after
// the download and before the decode, catches a heap that degraded during THIS cover's
// own download rather than a previous one's.
bool hasHeapForCoverDecode(const bool isPng) {
  // The JPEG decoder wants ~52KB of working buffers (JpegToBmpConverter's own
  // MIN_FREE_HEAP), so the pre-gate has to be met before it is worth starting.
  constexpr uint32_t JPEG_MIN_FREE_HEAP = 32 * 1024;
  constexpr uint32_t JPEG_MIN_FREE_BLOCK = 12 * 1024;

  // A PNG cover is a completely different proposition and must not be judged by the
  // JPEG numbers. Core serves the e-ink derivative as a 160x240 1-bit PNG, whose whole
  // decompressed payload is (160/8 + 1) * 240 = 5,040 bytes -- and PngToBmpConverter
  // already falls back to a one-shot decode against the destination buffer when the
  // 32KB streaming ring buffer cannot be allocated, needing no window at all (its own
  // comment: "true for every OPDS cover -- the server caps these at 160x240, 1-bit").
  //
  // Judged by the JPEG thresholds, that 4KB decode was refused at 30,312 bytes free --
  // 2,456 short of a limit sized for a decoder it never reaches. Every allocation on the
  // PNG path is nothrow and null-checked, and the one-shot fallback is itself capped at
  // 24,576 bytes, so an over-large PNG still fails cleanly rather than crashing. The
  // pre-gate here only has to be generous enough not to start work that cannot finish.
  // 6KB was wrong and cost a download and a TLS handshake per cover before
  // failing. The one-shot fallback avoids the 32KB *window*, but InflateStream
  // still needs a tinfl_decompressor -- about 11KB -- and it only comes free
  // during a framebuffer loan (buildscratch::claim), which is a chapter-build
  // thing and never held while browsing a catalogue. So a cover decode needs
  // ~11KB CONTIGUOUS, whatever the total free heap says.
  //
  // Measured on an X3 browsing the Arabic store: free 26,624 with a largest
  // block of 8,180. The old gate passed, the 4KB output buffer allocated fine,
  // and init then failed on the 11KB state -- reported only as the generic
  // "Failed to convert cover to BMP", after the bytes were already fetched.
  constexpr uint32_t PNG_MIN_FREE_HEAP = 16 * 1024;
  constexpr uint32_t PNG_MIN_FREE_BLOCK = 13 * 1024;

  const uint32_t minFree = isPng ? PNG_MIN_FREE_HEAP : JPEG_MIN_FREE_HEAP;
  const uint32_t minBlock = isPng ? PNG_MIN_FREE_BLOCK : JPEG_MIN_FREE_BLOCK;
  return ESP.getFreeHeap() > minFree && ESP.getMaxAllocHeap() > minBlock;
}
}  // namespace

std::string getOpdsCoverCachePath(const std::string& entryId, int width, int height) {
  return OpdsCoverCachePath::build(entryId, width, height);
}

bool ensureOpdsCoverCached(const OpdsEntry& entry, const std::string& username, const std::string& password, int width,
                           int height) {
  if (entry.coverUrl.empty()) {
    return false;
  }

  // Route CLASS, computed once. Nothing below logs the URL itself: a cover href is
  // server-chosen and may carry signed query parameters, so only the classification
  // (derivative vs full-size original) is ever written to serial or to the SD log.
  const bool einkRoute = entry.coverUrl.find("/cover/eink") != std::string::npos;

  const std::string cachePath = getOpdsCoverCachePath(entry.id, width, height);
  if (Storage.exists(cachePath.c_str())) {
    // Already cached -- but confirm it's a complete, well-formed BMP first. A partial file
    // left behind by an interrupted download/conversion would otherwise look "already cached"
    // forever and never get a chance to re-download.
    if (Bitmap::isValidCachedBmp(cachePath)) return true;
    LOG_ERR("OPDSCOVER", "Cached cover BMP is corrupt, re-fetching: %s", cachePath.c_str());
    Storage.remove(cachePath.c_str());
  }

  Storage.mkdir(OpdsCoverCachePath::DIR);

  // Downloaded once at a time (loadGridPageCovers walks covers sequentially),
  // so a single fixed temp filename is safe — no concurrent writers.
  const auto downloadResult =
      HttpDownloader::downloadToFile(entry.coverUrl, TEMP_PATH, nullptr, nullptr, username, password);
  if (downloadResult != HttpDownloader::OK) {
    LOG_ERR("OPDSCOVER", "Failed to download cover: route=%s", einkRoute ? "eink" : "full");
    return false;
  }

  // Opened and sniffed BEFORE the heap gate: what this decode costs depends entirely on
  // the format, and judging a 4KB PNG by the JPEG decoder's requirements is what kept
  // Core's e-ink derivative from ever being decoded.
  HalFile src;
  if (!Storage.openFileForRead("OPDSCOVER", TEMP_PATH, src)) {
    Storage.remove(TEMP_PATH);
    return false;
  }
  const bool isPng = downloadedCoverIsPng(src, entry.coverType);
  LOG_INF("OPDSCOVER", "cover route=%s bytes=%u fmt=%s free=%u largest=%u", einkRoute ? "eink" : "full",
          static_cast<unsigned>(src.fileSize()), isPng ? "png" : "jpeg", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));

  if (!hasHeapForCoverDecode(isPng)) {
    // The download itself (HTTP/TLS buffers, chunked reads) can fragment the heap enough
    // to make the decode below -- or even the string work already ahead of it -- unsafe,
    // regardless of how healthy heap looked when the caller checked before starting this
    // cover. Bail before touching the decoder; the downloaded temp file is discarded, and
    // this cover is simply left uncached for a later, healthier visit (same trade
    // loadGridPageCovers already makes for its own pre-loop check).
    LOG_ERR("OPDSCOVER", "Skipping decode, low heap after download: free=%u largest=%u",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    // Forced past the debugLoggingEnabled gate (see RollingSdLog::append) -- this is the
    // exact defensive bail this fix exists for (OpdsBookBrowserActivity.cpp's
    // hasHeapForNavigation()/hasHeapForCoverWork() comments have the full crash history),
    // and a near-miss save is worth as much locally as the crash it prevented. Formatted
    // into a stack buffer and written through appendRaw(): the std::string concatenation
    // this replaces allocated on the way to a logger that refuses to write below 32KB
    // free, i.e. it was most likely to be dropped exactly when it mattered.
    char line[224];
    const int len = snprintf(line, sizeof(line),
                             "[OPDSCOVER] Skipping decode, low heap after download: free=%u largest=%u route=%s fmt=%s",
                             static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
                             einkRoute ? "eink" : "full", isPng ? "png" : "jpeg");
    if (len > 0) {
      RollingSdLog::appendRaw(DebugLog::PATH, line, std::min<size_t>(static_cast<size_t>(len), sizeof(line) - 1),
                              DebugLog::MAX_LINES, /*force=*/true);
    }
    src.close();
    Storage.remove(TEMP_PATH);
    return false;
  }

  HalFile dst;
  if (!Storage.openFileForWrite("OPDSCOVER", cachePath, dst)) {
    // Close before removing the path this handle still holds open.
    src.close();
    Storage.remove(TEMP_PATH);
    return false;
  }

  // allowUpscale=false: the server already serves a source cover smaller than width/height at
  // its own native size rather than padding/stretching it, so upscaling it again here would
  // just blur it for no benefit -- the draw call centers whatever (possibly smaller) size comes
  // out of this instead.
  const bool success = isPng ? PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, width, height,
                                                                                 /*allowUpscale=*/false)
                             : JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, width, height);

  // Explicitly close() files before Storage.remove() on the same paths.
  src.close();
  dst.close();
  Storage.remove(TEMP_PATH);

  if (!success) {
    LOG_ERR("OPDSCOVER", "Failed to convert cover to BMP: route=%s fmt=%s", einkRoute ? "eink" : "full",
            isPng ? "png" : "jpeg");
    Storage.remove(cachePath.c_str());
  }
  return success;
}

bool clearOpdsCoverCache() {
  if (!Storage.exists(OpdsCoverCachePath::DIR)) return true;  // nothing to clear
  const bool removed = Storage.removeDir(OpdsCoverCachePath::DIR);
  if (!removed) {
    LOG_ERR("OPDSCOVER", "Failed to remove cover cache dir: %s", OpdsCoverCachePath::DIR);
  }
  return removed;
}
