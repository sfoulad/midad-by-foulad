#include "OpdsCoverCache.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <OpdsParser.h>
#include <PngToBmpConverter.h>

#include "network/HttpDownloader.h"
#include "util/DebugLog.h"
#include "util/RollingSdLog.h"

namespace {
constexpr char CACHE_DIR[] = "/.crosspoint/opds_covers";
constexpr char TEMP_PATH[] = "/.crosspoint/opds_covers/.tmp_cover";

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
bool hasHeapForCoverDecode() {
  constexpr uint32_t COVER_MIN_FREE_HEAP = 32 * 1024;
  constexpr uint32_t COVER_MIN_FREE_BLOCK = 12 * 1024;
  return ESP.getFreeHeap() > COVER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > COVER_MIN_FREE_BLOCK;
}
}  // namespace

std::string getOpdsCoverCachePath(const std::string& entryId, int width, int height) {
  const size_t hash = std::hash<std::string>{}(entryId);
  return std::string(CACHE_DIR) + "/" + std::to_string(hash) + "_" + std::to_string(width) + "x" +
         std::to_string(height) + ".bmp";
}

bool ensureOpdsCoverCached(const OpdsEntry& entry, const std::string& username, const std::string& password, int width,
                           int height) {
  if (entry.coverUrl.empty()) {
    return false;
  }

  const std::string cachePath = getOpdsCoverCachePath(entry.id, width, height);
  if (Storage.exists(cachePath.c_str())) {
    // Already cached -- but confirm it's a complete, well-formed BMP first. A partial file
    // left behind by an interrupted download/conversion would otherwise look "already cached"
    // forever and never get a chance to re-download.
    if (Bitmap::isValidCachedBmp(cachePath)) return true;
    LOG_ERR("OPDSCOVER", "Cached cover BMP is corrupt, re-fetching: %s", cachePath.c_str());
    Storage.remove(cachePath.c_str());
  }

  Storage.mkdir(CACHE_DIR);

  // Downloaded once at a time (loadGridPageCovers walks covers sequentially),
  // so a single fixed temp filename is safe — no concurrent writers.
  const auto downloadResult =
      HttpDownloader::downloadToFile(entry.coverUrl, TEMP_PATH, nullptr, nullptr, username, password);
  if (downloadResult != HttpDownloader::OK) {
    LOG_ERR("OPDSCOVER", "Failed to download cover: %s", entry.coverUrl.c_str());
    return false;
  }

  if (!hasHeapForCoverDecode()) {
    // The download itself (HTTP/TLS buffers, chunked reads) can fragment the heap enough
    // to make the decode below -- or even the string work already ahead of it -- unsafe,
    // regardless of how healthy heap looked when the caller checked before starting this
    // cover. Bail before touching the decoder; the downloaded temp file is discarded, and
    // this cover is simply left uncached for a later, healthier visit (same trade
    // loadGridPageCovers already makes for its own pre-loop check).
    LOG_ERR("OPDSCOVER", "Skipping decode, low heap after download: free=%u largest=%u",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    // Forced through the debugLoggingEnabled gate (see RollingSdLog::append) -- this is
    // the exact defensive bail this fix exists for (OpdsBookBrowserActivity.cpp's
    // hasHeapForNavigation()/hasHeapForCoverWork() comments have the full crash history),
    // and a near-miss save is worth as much locally as the crash it prevented.
    RollingSdLog::append(
        DebugLog::PATH,
        "[OPDSCOVER] Skipping decode, low heap after download: free=" + std::to_string(ESP.getFreeHeap()) +
            " largest=" + std::to_string(ESP.getMaxAllocHeap()) + " url=" + entry.coverUrl,
        DebugLog::MAX_LINES, /*force=*/true);
    Storage.remove(TEMP_PATH);
    return false;
  }

  HalFile src;
  if (!Storage.openFileForRead("OPDSCOVER", TEMP_PATH, src)) {
    Storage.remove(TEMP_PATH);
    return false;
  }

  HalFile dst;
  if (!Storage.openFileForWrite("OPDSCOVER", cachePath, dst)) {
    Storage.remove(TEMP_PATH);
    return false;
  }

  // allowUpscale=false: the server already serves a source cover smaller than width/height at
  // its own native size rather than padding/stretching it, so upscaling it again here would
  // just blur it for no benefit -- the draw call centers whatever (possibly smaller) size comes
  // out of this instead.
  const bool isPng = entry.coverType.find("png") != std::string::npos;
  const bool success = isPng ? PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, width, height,
                                                                                 /*allowUpscale=*/false)
                             : JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, width, height);

  // Explicitly close() files before Storage.remove() on the same paths.
  src.close();
  dst.close();
  Storage.remove(TEMP_PATH);

  if (!success) {
    LOG_ERR("OPDSCOVER", "Failed to convert cover to BMP: %s", entry.coverUrl.c_str());
    Storage.remove(cachePath.c_str());
  }
  return success;
}

bool clearOpdsCoverCache() {
  if (!Storage.exists(CACHE_DIR)) return true;  // nothing to clear
  const bool removed = Storage.removeDir(CACHE_DIR);
  if (!removed) {
    LOG_ERR("OPDSCOVER", "Failed to remove cover cache dir: %s", CACHE_DIR);
  }
  return removed;
}
