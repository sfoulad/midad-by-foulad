#include "OpdsCoverCache.h"

#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <OpdsParser.h>
#include <PngToBmpConverter.h>

#include "network/HttpDownloader.h"

namespace {
constexpr char CACHE_DIR[] = "/.crosspoint/opds_covers";
constexpr char TEMP_PATH[] = "/.crosspoint/opds_covers/.tmp_cover";
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
    return true;
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

  const bool isPng = entry.coverType.find("png") != std::string::npos;
  const bool success = isPng ? PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, width, height)
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
