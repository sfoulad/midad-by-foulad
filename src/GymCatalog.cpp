#include "GymCatalog.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>

#include "GymCatalogJsonParser.h"

namespace GymCatalog {

namespace {
// Matches HttpDownloader's own chunked-read buffer size for this class of
// operation (see src/network/HttpDownloader.cpp) -- heap-allocated, not a
// stack local, per the project's stack-safety convention.
constexpr size_t READ_CHUNK = 2048;

// Opens CATALOG_PATH (or an arbitrary path) and feeds it through `parser` in
// READ_CHUNK pieces. Returns false (and leaves `parser` however the caller's
// mode left it) if the file can't be opened or the read buffer can't be
// allocated.
bool feedFile(const std::string& path, GymCatalogJsonParser& parser) {
  HalFile file;
  if (!Storage.openFileForRead("GYM", path.c_str(), file)) {
    return false;  // Not downloaded yet -- expected until the user runs the download store.
  }

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("GYM", "OOM: %u byte read buffer", static_cast<unsigned>(READ_CHUNK));
    return false;
  }

  int bytesRead;
  while ((bytesRead = file.read(buf.get(), READ_CHUNK)) > 0) {
    parser.feed(buf.get(), static_cast<size_t>(bytesRead));
  }
  return true;
}
}  // namespace

bool validate(const std::string& path, size_t& countOut) {
  countOut = 0;
  GymCatalogJsonParser parser(static_cast<std::deque<GymCatalogEntry>*>(nullptr));
  if (!feedFile(path, parser)) return false;

  if (!parser.hasVersion() || parser.getVersion() != 1) {
    LOG_ERR("GYM", "Unsupported or missing gym catalog version: %d", parser.getVersion());
    return false;
  }
  countOut = parser.getCount();
  return true;
}

bool loadBodyPartCounts(std::vector<GymBodyPartCount>& out) {
  out.clear();
  GymCatalogJsonParser parser(&out);
  if (!feedFile(CATALOG_PATH, parser)) return false;

  if (!parser.hasVersion() || parser.getVersion() != 1) {
    LOG_ERR("GYM", "Unsupported or missing gym catalog version: %d", parser.getVersion());
    out.clear();
    return false;
  }
  LOG_DBG("GYM", "Loaded %u body parts (%u exercises total)", static_cast<unsigned>(out.size()),
          static_cast<unsigned>(parser.getCount()));
  return true;
}

bool loadExercisesForBodyPart(const std::string& bodyPart, std::deque<GymCatalogEntry>& out) {
  out.clear();
  GymCatalogJsonParser parser(&out, bodyPart);
  if (!feedFile(CATALOG_PATH, parser)) return false;

  if (!parser.hasVersion() || parser.getVersion() != 1) {
    LOG_ERR("GYM", "Unsupported or missing gym catalog version: %d", parser.getVersion());
    out.clear();
    return false;
  }
  LOG_DBG("GYM", "Loaded %u exercises for bodyPart=%s", static_cast<unsigned>(out.size()), bodyPart.c_str());
  return true;
}

std::string imagePath(const std::string& slug) { return "/gym/images/" + slug + ".jpg"; }

std::string instructionsPath(const std::string& slug) { return "/gym/instructions/" + slug + ".json"; }

std::string imageThumbPath(const std::string& slug, const int maxWidth, const int maxHeight) {
  char suffix[32];
  snprintf(suffix, sizeof(suffix), "_bw_%dx%d.bmp", maxWidth, maxHeight);
  return "/gym/images/" + slug + suffix;
}

bool ensureImageThumb(const std::string& slug, const int maxWidth, const int maxHeight) {
  const std::string thumbPath = imageThumbPath(slug, maxWidth, maxHeight);
  if (Bitmap::isValidCachedBmp(thumbPath)) return true;

  const std::string jpgPath = imagePath(slug);
  HalFile jpgFile;
  if (!Storage.openFileForRead("GYM", jpgPath.c_str(), jpgFile)) {
    LOG_ERR("GYM", "Cannot open source image for thumb: %s", jpgPath.c_str());
    return false;
  }

  HalFile thumbFile;
  if (!Storage.openFileForWrite("GYM", thumbPath.c_str(), thumbFile)) {
    LOG_ERR("GYM", "Cannot open thumb BMP for write: %s", thumbPath.c_str());
    return false;
  }

  if (!JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpgFile, thumbFile, maxWidth, maxHeight)) {
    LOG_ERR("GYM", "Failed to generate thumb BMP for %s", slug.c_str());
    thumbFile.close();
    Storage.remove(thumbPath.c_str());
    return false;
  }
  // Close before Bitmap::isValidCachedBmp reopens it for read below.
  thumbFile.close();

  if (!Bitmap::isValidCachedBmp(thumbPath)) {
    LOG_ERR("GYM", "Generated thumb BMP failed validation: %s", thumbPath.c_str());
    Storage.remove(thumbPath.c_str());
    return false;
  }
  return true;
}

}  // namespace GymCatalog
