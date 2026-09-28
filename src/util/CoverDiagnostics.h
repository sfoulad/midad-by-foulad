#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Pure (Arduino-free, SD-free) half of the book-cover thumbnail pipeline, split
// out so the decisions that decide whether a cover paints can be exercised on
// the host and so a failure names itself in the log instead of degrading into
// the same silent placeholder for every possible cause.
//
// Before this existed, a Home/My Books cover that failed to appear produced NO
// log line at all: FouladTheme::drawCoverAt opened the file, and on any failure
// (missing, unparseable, wrong size, allocation failure inside the blit) fell
// through to the identical black-block placeholder. "No cover art", "stale
// cache", "out of memory" and "the renderer skipped the draw" were
// indistinguishable from a photo of the screen, which is exactly how an X4 Pro
// report ended up being diagnosed by guesswork.
namespace CoverDiag {

// Why a cover box did not receive its bitmap. Ordered so the first cause found
// while walking the pipeline wins; classify() below encodes that order.
enum class Fault : uint8_t {
  None = 0,     // the cover painted
  NoPath,       // the book entry carries no cover-thumb template at all
  Missing,      // the thumb file could not be opened -- never generated, or removed
  Invalid,      // the file exists but is not a well-formed BMP (truncated, or the empty marker)
  StaleSize,    // a well-formed BMP, but not the height this screen asked to have generated
  OutOfMemory,  // the blit could not allocate its row/accumulator buffers
  NotPainted,   // the renderer accepted the call but painted nothing (e.g. a font-cache scan)
};

// Stable, greppable token for the log line. Never translated: this is
// diagnostic output, not user-facing text.
const char* faultName(Fault fault);

// Substitutes the "[HEIGHT]" placeholder that RecentBooksStore persists inside
// coverBmpPath (".../thumb_[HEIGHT].bmp") with a concrete height. A path with no
// placeholder is returned unchanged. Single implementation shared with
// UITheme::getCoverThumbPath so the resolved name can never drift between the
// screen that generates a thumb and the screen that reads it.
std::string thumbPathForHeight(std::string templatePath, int height);

// Recovers the height encoded in a ".../thumb_<N>.bmp" name, or 0 when the name
// is not a resolved thumb path (an unresolved "[HEIGHT]" template included).
// Lets a diagnostic say WHICH size was asked for without threading the request
// through every call site.
int heightFromThumbPath(const std::string& path);

// Result of reading a BMP file header. Mirrors the fields Bitmap::parseHeaders
// derives, including the negative-biHeight (top-down row order) convention the
// cover converters emit, so a diagnostic can report real dimensions for a file
// the renderer rejected.
struct BmpHeaderInfo {
  bool ok = false;
  int width = 0;
  int height = 0;  // always positive; see topDown for the stored sign
  bool topDown = false;
  uint16_t bpp = 0;
  uint32_t compression = 0;
  uint32_t dataOffset = 0;
  // Short reason the header was rejected; "" when ok.
  const char* reason = "";
};

// Parses `len` bytes from the start of a BMP file. 54 bytes is enough (file
// header + 40-byte DIB header); anything shorter is reported as truncated.
BmpHeaderInfo inspectBmpHeader(const uint8_t* data, size_t len);

// How a generated thumbnail is fitted into a cover box. Mirrors -- and is used
// by -- FouladTheme::drawCoverAt, so the geometry the tests pin is the geometry
// that actually runs.
//
// Two cases, because thumbs are generated at ONE canonical height (the theme's
// hero height) and reused by every smaller tile:
//   exactHeight: the thumb is exactly as tall as the box, so it is cropped
//                horizontally to fill the box edge-to-edge.
//   shrink-to-fit: a taller thumb reused in a smaller tile is scaled down
//                  (never up -- drawBitmap only scales down) and centered.
struct CoverFit {
  bool valid = false;  // false when any input dimension is non-positive
  bool exactHeight = false;
  float cropX = 0.0f;  // horizontal crop fraction handed to drawBitmap
  float scale = 1.0f;  // 1.0 in the exact-height case
  int drawWidth = 0;   // painted extent, clamped to the box
  int drawHeight = 0;
  int offsetX = 0;  // centering offset inside the box
  int offsetY = 0;
};

CoverFit fitCoverIntoBox(int srcWidth, int srcHeight, int boxWidth, int boxHeight);

// Collapses the observations made while loading one cover into a single cause.
// `expectedHeight <= 0` disables the stale-size check (callers that legitimately
// accept any height).
Fault classify(bool hasPath, bool opened, bool headerOk, int bitmapWidth, int bitmapHeight, int expectedHeight,
               bool allocOk, bool painted);

}  // namespace CoverDiag
