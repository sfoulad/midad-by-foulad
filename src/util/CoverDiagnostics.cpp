#include "util/CoverDiagnostics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace CoverDiag {
namespace {

constexpr char kHeightPlaceholder[] = "[HEIGHT]";
constexpr size_t kHeightPlaceholderLen = sizeof(kHeightPlaceholder) - 1;
constexpr char kThumbPrefix[] = "thumb_";
constexpr size_t kThumbPrefixLen = sizeof(kThumbPrefix) - 1;
constexpr char kBmpSuffix[] = ".bmp";
constexpr size_t kBmpSuffixLen = sizeof(kBmpSuffix) - 1;

// Same limits Bitmap::parseHeaders enforces, repeated here so a diagnostic
// reports the same verdict the renderer would reach.
constexpr int kMaxImageWidth = 2048;
constexpr int kMaxImageHeight = 3072;

uint16_t readLE16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8); }

uint32_t readLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

const char* faultName(const Fault fault) {
  switch (fault) {
    case Fault::None:
      return "OK";
    case Fault::NoPath:
      return "NO-COVER-PATH";
    case Fault::Missing:
      return "MISSING-COVER";
    case Fault::Invalid:
      return "INVALID-CACHE";
    case Fault::StaleSize:
      return "STALE-CACHE-SIZE";
    case Fault::OutOfMemory:
      return "OOM";
    case Fault::NotPainted:
      return "DRAW-SKIPPED";
  }
  return "UNKNOWN";
}

std::string thumbPathForHeight(std::string templatePath, const int height) {
  const size_t pos = templatePath.find(kHeightPlaceholder, 0);
  if (pos != std::string::npos) {
    templatePath.replace(pos, kHeightPlaceholderLen, std::to_string(height));
  }
  return templatePath;
}

int heightFromThumbPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
  const size_t nameLen = path.size() - nameStart;
  if (nameLen <= kThumbPrefixLen + kBmpSuffixLen) return 0;
  if (path.compare(nameStart, kThumbPrefixLen, kThumbPrefix) != 0) return 0;
  if (path.compare(path.size() - kBmpSuffixLen, kBmpSuffixLen, kBmpSuffix) != 0) return 0;

  const size_t digitsStart = nameStart + kThumbPrefixLen;
  const size_t digitsLen = path.size() - kBmpSuffixLen - digitsStart;
  if (digitsLen == 0 || digitsLen > 6) return 0;  // 6 digits is far past any panel height

  int height = 0;
  for (size_t i = 0; i < digitsLen; i++) {
    const char c = path[digitsStart + i];
    if (c < '0' || c > '9') return 0;  // an unresolved "[HEIGHT]" template lands here
    height = height * 10 + (c - '0');
  }
  return height;
}

BmpHeaderInfo inspectBmpHeader(const uint8_t* data, const size_t len) {
  BmpHeaderInfo info;
  if (data == nullptr || len < 54) {
    info.reason = "truncated (<54 bytes)";
    return info;
  }
  if (readLE16(data) != 0x4D42) {
    info.reason = "not a BMP (missing 'BM')";
    return info;
  }
  info.dataOffset = readLE32(data + 10);

  const uint32_t dibSize = readLE32(data + 14);
  if (dibSize < 40) {
    info.reason = "DIB header too small";
    return info;
  }

  const auto width = static_cast<int32_t>(readLE32(data + 18));
  const auto rawHeight = static_cast<int32_t>(readLE32(data + 22));
  info.width = width;
  // INT32_MIN has no positive counterpart, so negating it below would be signed
  // overflow (UB) -- rejected here rather than at the dimension check further
  // down, which never gets to run. Same guard as SleepActivity's overlay header
  // reader.
  if (rawHeight == std::numeric_limits<int32_t>::min()) {
    info.reason = "bad dimensions";
    return info;
  }
  // Negative biHeight means top-down row order, which is exactly what the cover
  // converters emit. Recording the flag (rather than treating the sign as
  // corruption) is what keeps a valid top-down thumb from being thrown away.
  info.topDown = rawHeight < 0;
  info.height = info.topDown ? -rawHeight : rawHeight;

  const uint16_t planes = readLE16(data + 26);
  info.bpp = readLE16(data + 28);
  info.compression = readLE32(data + 30);

  if (planes != 1) {
    info.reason = "planes != 1";
    return info;
  }
  const bool validBpp =
      info.bpp == 1 || info.bpp == 2 || info.bpp == 4 || info.bpp == 8 || info.bpp == 24 || info.bpp == 32;
  if (!validBpp) {
    info.reason = "unsupported bit depth";
    return info;
  }
  if (!(info.compression == 0 || (info.bpp == 32 && info.compression == 3))) {
    info.reason = "compressed BMP";
    return info;
  }
  if (info.width <= 0 || info.height <= 0) {
    info.reason = "bad dimensions";
    return info;
  }
  if (info.width > kMaxImageWidth || info.height > kMaxImageHeight) {
    info.reason = "image too large";
    return info;
  }

  info.ok = true;
  return info;
}

CoverFit fitCoverIntoBox(const int srcWidth, const int srcHeight, const int boxWidth, const int boxHeight) {
  CoverFit fit;
  if (srcWidth <= 0 || srcHeight <= 0 || boxWidth <= 0 || boxHeight <= 0) return fit;
  fit.valid = true;

  if (srcHeight == boxHeight) {
    // Exact-height thumb (the hero): crop horizontally so the art fills the box
    // edge to edge instead of leaving side gutters.
    fit.exactHeight = true;
    const float ratio = static_cast<float>(srcWidth) / static_cast<float>(srcHeight);
    const float tileRatio = static_cast<float>(boxWidth) / static_cast<float>(boxHeight);
    fit.cropX = std::max(0.0f, 1.0f - (tileRatio / ratio));
    // drawBitmap drops floor(width * cropX / 2) columns from EACH side.
    const int cropPixX = static_cast<int>(std::floor(srcWidth * fit.cropX / 2.0f));
    fit.drawWidth = std::min(boxWidth, srcWidth - 2 * cropPixX);
    fit.drawHeight = srcHeight;
    return fit;
  }

  // Larger thumb reused for a smaller tile: shrink to fit, centered. drawBitmap
  // never scales UP, so a thumb smaller than the box keeps its native size.
  const float widthScale = static_cast<float>(boxWidth) / static_cast<float>(srcWidth);
  const float heightScale = static_cast<float>(boxHeight) / static_cast<float>(srcHeight);
  fit.scale = std::min(1.0f, std::min(widthScale, heightScale));
  const int scaledW = static_cast<int>(srcWidth * fit.scale);
  const int scaledH = static_cast<int>(srcHeight * fit.scale);
  fit.offsetX = std::max(0, (boxWidth - scaledW) / 2);
  fit.offsetY = std::max(0, (boxHeight - scaledH) / 2);
  if (fit.scale < 1.0f) {
    // Matches drawBitmap1Bit's destination extent exactly.
    fit.drawWidth = static_cast<int>(std::floor((srcWidth - 1) * fit.scale)) + 1;
    fit.drawHeight = static_cast<int>(std::floor((srcHeight - 1) * fit.scale)) + 1;
  } else {
    fit.drawWidth = srcWidth;
    fit.drawHeight = srcHeight;
  }
  return fit;
}

Fault classify(const bool hasPath, const bool opened, const bool headerOk, const int bitmapWidth,
               const int bitmapHeight, const int expectedHeight, const bool allocOk, const bool painted) {
  if (!hasPath) return Fault::NoPath;
  if (!opened) return Fault::Missing;
  if (!headerOk || bitmapWidth <= 0 || bitmapHeight <= 0) return Fault::Invalid;
  if (expectedHeight > 0 && bitmapHeight != expectedHeight) return Fault::StaleSize;
  if (!allocOk) return Fault::OutOfMemory;
  if (!painted) return Fault::NotPainted;
  return Fault::None;
}

}  // namespace CoverDiag
