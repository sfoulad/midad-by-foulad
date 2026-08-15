#include "GfxRenderer.h"

#include <ArabicShaper.h>
#include <BidiUtils.h>
#include <BuildScratch.h>
#include <FontDecompressor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <ScriptDetector.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <string_view>

#include "FontCacheManager.h"

namespace {

/**
 * Resolves the requested style to the best available style in the given SD card font.
 * Falls back gracefully when the font lacks the requested variant.
 */
uint8_t resolveSdCardStyle(const SdCardFont& font, const EpdFontFamily::Style style) {
  return font.resolveStyle(static_cast<uint8_t>(style));
}
}  // namespace

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, BidiUtils::BidiBaseDir baseDir);

// Appends the shaped visual form of every RTL token in `text` to `shapedOut`.
// getTextAdvanceX() measures the bidi-reordered, Arabic-shaped codepoint stream,
// so the SD advance table must be warmed with the presentation forms as well as
// the logical codepoints — otherwise every RTL word measurement misses the fast
// path and falls through to onGlyphMiss(), which opens the .cpfont and reads
// glyph metadata + bitmap into the 8-slot overflow ring, once per glyph.
// Tokens without RTL lead bytes (0xD6-0xDB) are skipped with a byte scan, so
// pure-LTR text pays almost nothing.
void appendShapedRtlTokens(const char* text, std::string& shapedOut) {
  const auto isBreak = [](const char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
  std::string token;
  std::string visual;
  const char* p = text;
  while (*p) {
    while (*p && isBreak(*p)) ++p;
    const char* start = p;
    bool hasRtlBytes = false;
    while (*p && !isBreak(*p)) {
      const auto b = static_cast<unsigned char>(*p);
      hasRtlBytes = hasRtlBytes || (b >= 0xD6 && b <= 0xDB);
      ++p;
    }
    if (!hasRtlBytes) continue;
    token.assign(start, p - start);
    if (BidiUtils::applyBidiVisual(token.c_str(), visual, static_cast<int>(BidiUtils::BidiBaseDir::AUTO))) {
      shapedOut += visual;
    }
  }
}
}  // namespace

const uint8_t* GfxRenderer::getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const {
  if (fontData->groups != nullptr) {
    auto* fd = fontCacheManager_ ? fontCacheManager_->getDecompressor() : nullptr;
    if (!fd) {
      LOG_ERR("GFX", "Compressed font but no FontDecompressor set");
      return nullptr;
    }
    uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
    // For page-buffer hits the pointer is stable for the page lifetime.
    // For hot-group hits it is valid only until the next getBitmap() call — callers
    // must consume it (draw the glyph) before requesting another bitmap.
    return fd->getBitmap(fontData, glyph, glyphIndex);
  }
  // For SD card fonts, check if the glyph was loaded on demand into the overflow
  // buffer.  getOverflowBitmap() returns:
  //   - bitmap pointer for overflow glyphs with bitmap data
  //   - nullptr for overflow glyphs without bitmap data (e.g. space: width=0, height=0)
  //   - nullptr for non-overflow glyphs (normal prewarmed path)
  // We distinguish overflow-with-no-bitmap from non-overflow by checking isOverflowGlyph().
  if (fontData->glyphMissCtx) {
    auto* sdFont = SdCardFont::fromMissCtx(fontData->glyphMissCtx);
    if (sdFont->isOverflowGlyph(glyph)) {
      return sdFont->getOverflowBitmap(glyph);  // may be nullptr for zero-width glyphs
    }
  }
  return &fontData->bitmap[glyph->dataOffset];
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    std::string shaped;
    appendShapedRtlTokens(utf8Text, shaped);
    int missed = it->second->buildAdvanceTable(utf8Text, styleMask, shaped.empty() ? nullptr : shaped.c_str());
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

void GfxRenderer::ensureSdCardFontReady(int fontId, const std::deque<std::string>& words, bool includeHyphen,
                                        uint8_t styleMask) const {
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    // Augment the persistent advance-only table for layout measurement.
    // The table survives across paragraphs/sections (capped per font), so
    // repeated indexing of the same SD font amortizes glyph-metric SD reads.
    std::string shaped;
    for (const auto& w : words) {
      appendShapedRtlTokens(w.c_str(), shaped);
    }
    int missed =
        it->second->buildAdvanceTable(words, includeHyphen, styleMask, shaped.empty() ? nullptr : shaped.c_str());
    if (missed > 0) {
      LOG_DBG("GFX", "ensureSdCardFontReady: %d glyph(s) not found", missed);
    }
  }
}

void GfxRenderer::begin() {
  frameBuffer = display.getFrameBuffer();
  if (!frameBuffer) {
    LOG_ERR("GFX", "!! No framebuffer");
    assert(false);
  }
  panelWidth = display.getDisplayWidth();
  panelHeight = display.getDisplayHeight();
  panelWidthBytes = display.getDisplayWidthBytes();
  frameBufferSize = display.getBufferSize();
  bwBufferChunks.assign((frameBufferSize + BW_BUFFER_CHUNK_SIZE - 1) / BW_BUFFER_CHUNK_SIZE, nullptr);
}

void GfxRenderer::releaseFrameBufferForBuild() {
  // Lend the framebuffer's bytes IN PLACE: the allocation is never freed, so
  // it cannot move and repeated loans cannot fragment the heap (the previous
  // free+realloc model measurably decayed the max contiguous block over a
  // session). The bytes are deposited in the build-scratch registry so
  // memory-hungry build phases (e.g. InflateStream's tinfl state + window)
  // can claim them instead of allocating.
  uint32_t size = 0;
  uint8_t* scratch = display.lendFrameBufferStorage(&size);
  frameBuffer = nullptr;
  if (scratch) {
    buildscratch::lend(scratch, size);
  }
}

bool GfxRenderer::restoreFrameBufferAfterBuild() {
  buildscratch::reclaim();
  display.returnFrameBufferStorage();  // cannot fail: the allocation was never freed
  frameBuffer = display.getFrameBuffer();
  return frameBuffer != nullptr;
}

GfxRenderer::FrameBufferLoan::FrameBufferLoan(GfxRenderer& renderer) : renderer_(renderer) {
  // Nesting guard: if the framebuffer is already lent out (an outer loan),
  // stay inert so this end() cannot return storage the outer loan still owns.
  if (!renderer_.hasFrameBuffer()) return;
  renderer_.releaseFrameBufferForBuild();
  active_ = true;
}

void GfxRenderer::FrameBufferLoan::end() {
  if (!active_) return;
  active_ = false;
  if (!renderer_.restoreFrameBufferAfterBuild()) {
    // Only reachable if the framebuffer never existed, which begin() already
    // asserts against; kept as a backstop since running blind helps nobody.
    LOG_ERR("GFX", "Framebuffer restore failed - restarting");
    ESP.restart();
  }
}

bool GfxRenderer::isFontCacheScanning() const { return fontCacheManager_ && fontCacheManager_->isScanning(); }

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) {
  auto result = fontMap.insert({fontId, font});
  if (!result.second) {
    LOG_ERR("GFX", "Font ID %d already registered, ignoring duplicate", fontId);
  }
}

int GfxRenderer::resolveTextFontId(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (fallbackFontMap_.empty() || text == nullptr || *text == '\0') {
    return fontId;
  }
  const auto fbIt = fallbackFontMap_.find(fontId);
  if (fbIt == fallbackFontMap_.end()) {
    return fontId;  // no fallback registered for this font
  }
  const int fallbackFontId = fbIt->second;
  const auto fontIt = fontMap.find(fontId);
  const auto fallbackIt = fontMap.find(fallbackFontId);
  if (fontIt == fontMap.end() || fallbackIt == fontMap.end()) {
    return fontId;  // unknown primary or fallback not loaded — let the caller handle it
  }
  const EpdFontFamily& primary = fontIt->second;
  const EpdFontFamily& fallback = fallbackIt->second;
  const char* cursor = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor)))) {
    // Only redirect for CJK the primary font cannot draw but the fallback can.
    // Latin/symbol strings the built-in UI fonts already cover are left
    // untouched, and a partial-coverage fallback (e.g. kana-only) is not worth
    // dragging the whole string into for glyphs it would also miss.
    if (utf8IsCjkCodepoint(cp) && !primary.hasCodepoint(cp, style) && fallback.hasCodepoint(cp, style)) {
      return fallbackFontId;
    }
  }
  return fontId;
}

void GfxRenderer::ensureSdGlyphsResident(const int fontId, const char* text, const EpdFontFamily::Style style,
                                         const bool metadataOnly) const {
  const auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt == sdCardFonts_.end()) {
    return;
  }
  // SUP/SUB bits don't select a distinct .cpfont style bitstream — mask to the
  // base style. resolveStyleMask() inside prewarm folds absent styles.
  const uint8_t styleMask = static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
  sdIt->second->prewarm(text, styleMask, metadataOnly);
}

// Translate logical (x,y) coordinates to physical panel coordinates based on current orientation
// This should always be inlined for better performance
static inline void rotateCoordinates(const GfxRenderer::Orientation orientation, const int x, const int y, int* phyX,
                                     int* phyY, const uint16_t panelWidth, const uint16_t panelHeight) {
  switch (orientation) {
    case GfxRenderer::Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    }
    case GfxRenderer::LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    }
    case GfxRenderer::PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    }
    case GfxRenderer::LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
    }
  }
}

// Output of screenRectToAlignedMemRect: a rectangle in panel-memory
// coordinates whose x and width are guaranteed to be multiples of 8 (the
// SDK's EInkDisplay::displayWindow alignment requirement). `valid == false`
// means the input was empty or fully outside the panel.
struct AlignedMemRect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  bool valid = false;
};

// Translate a screen-coordinate rectangle (the coordinate system used by
// fillRect / drawText / the rest of the renderer's public API) into a
// panel-memory rectangle suitable for direct framebuffer indexing. Rotates
// the rectangle's two opposite corners with rotateCoordinates(), takes the
// bounding box (which naturally swaps width/height in Portrait /
// PortraitInverted), then snaps the x extent outward to multiples of 8 and
// clamps to panel bounds. Precondition: panel dims are multiples of 8 (true
// for the 800x480 panel), so clamping cannot re-break alignment.
static AlignedMemRect screenRectToAlignedMemRect(GfxRenderer::Orientation orientation, int sx, int sy, int sw, int sh,
                                                 uint16_t panelWidth, uint16_t panelHeight) {
  AlignedMemRect out;
  if (sw <= 0 || sh <= 0) return out;

  int x0, y0, x1, y1;
  rotateCoordinates(orientation, sx, sy, &x0, &y0, panelWidth, panelHeight);
  rotateCoordinates(orientation, sx + sw - 1, sy + sh - 1, &x1, &y1, panelWidth, panelHeight);

  const int memXLo = std::min(x0, x1);
  const int memYLo = std::min(y0, y1);
  const int memXHi = std::max(x0, x1) + 1;  // exclusive upper bound
  const int memYHi = std::max(y0, y1) + 1;

  // Snap x outward to multiples of 8.
  int alignedXLo = memXLo & ~0x7;        // round down
  int alignedXHi = (memXHi + 7) & ~0x7;  // round up

  if (alignedXLo < 0) alignedXLo = 0;
  if (alignedXHi > panelWidth) alignedXHi = panelWidth;
  int clampedYLo = memYLo;
  int clampedYHi = memYHi;
  if (clampedYLo < 0) clampedYLo = 0;
  if (clampedYHi > panelHeight) clampedYHi = panelHeight;

  if (alignedXHi <= alignedXLo || clampedYHi <= clampedYLo) return out;

  out.x = static_cast<uint16_t>(alignedXLo);
  out.y = static_cast<uint16_t>(clampedYLo);
  out.w = static_cast<uint16_t>(alignedXHi - alignedXLo);
  out.h = static_cast<uint16_t>(clampedYHi - clampedYLo);
  out.valid = true;
  return out;
}

enum class TextRotation { None, Rotated90CW };

// Shared glyph rendering logic for normal and rotated text.
// Coordinate mapping and cursor advance direction are selected at compile time via the template parameter.
// Render a glyph at 50% scale. Used for SUP/SUB style bits.
//
// Each destination pixel represents a 2x2 source block. Drawing when that block
// contains ink preserves thin strokes that nearest-neighbor sampling can skip.
//
// The advance width is also halved in drawText() so layout reserves exactly the right
// horizontal space for the scaled glyph.
static void renderCharScaled(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                             const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                             const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) return;

  const EpdFontData* fontData = fontFamily.getData(style);
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (!bitmap) return;

  const int srcW = glyph->width;
  const int srcH = glyph->height;
  const int dstW = (srcW + 1) / 2;  // ceil so odd-width glyphs aren't clipped
  const int dstH = (srcH + 1) / 2;
  // Scale the glyph bearing by the same factor so the scaled glyph sits at the correct
  // pixel offset from the (already-shifted) cursor position.
  const int baseX = cursorX + glyph->left / 2;
  const int baseY = cursorY - glyph->top / 2;

  if (fontData->is2Bit) {
    // 2-bit packed format: 4 pixels per byte, MSB first, 2 bits per pixel.
    // raw value: 0=white, 1=light-gray, 2=dark-gray, 3=black.
    for (int dstY = 0; dstY < dstH; dstY++) {
      const int srcY = dstY * 2;
      for (int dstX = 0; dstX < dstW; dstX++) {
        const int srcX = dstX * 2;
        uint8_t coverage = 0;
        uint8_t maxRaw = 0;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            const uint8_t byte = bitmap[pos >> 2];
            const uint8_t raw = (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
            coverage += raw;
            if (raw > maxRaw) maxRaw = raw;
          }
        }
        if (maxRaw >= 2 || coverage >= 2) {
          renderer.drawPixel(baseX + dstX, baseY + dstY, pixelState);
        }
      }
    }
  } else {
    // 1-bit packed format: 8 pixels per byte, MSB first.
    for (int dstY = 0; dstY < dstH; dstY++) {
      const int srcY = dstY * 2;
      for (int dstX = 0; dstX < dstW; dstX++) {
        const int srcX = dstX * 2;
        bool hasInk = false;
        for (int sampleY = 0; sampleY < 2 && srcY + sampleY < srcH; sampleY++) {
          for (int sampleX = 0; sampleX < 2 && srcX + sampleX < srcW; sampleX++) {
            const int pos = (srcY + sampleY) * srcW + srcX + sampleX;
            const uint8_t byte = bitmap[pos >> 3];
            const uint8_t bit = 7 - (pos & 7);
            if ((byte >> bit) & 1) {
              hasInk = true;
            }
          }
        }
        if (hasInk) {
          renderer.drawPixel(baseX + dstX, baseY + dstY, pixelState);
        }
      }
    }
  }
}

// Render a glyph at an arbitrary rational up-scale (scaleNum/scaleDen, e.g.
// 3/2 for 1.5x) via nearest-neighbor sampling -- inverse of renderCharScaled
// above (which only ever halves, for SUP/SUB). Used specifically for the
// Bismillah ligature glyph, whose baked size is capped by EpdGlyph::width/
// height being uint8_t (255px): quran-common.ttf's single whole-phrase glyph
// is already 241px wide at the largest safely-generatable size (18pt, see
// quran_common's conversion comment in convert-builtin-fonts.sh), leaving no
// room to bake it any bigger, so this scales the existing bitmap up at draw
// time instead. Acceptable for a single, once-per-surah glyph, unlike
// per-character body text where blocky upscaling would look worse than just
// using a bigger font.
static void renderCharUpscaled(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                               const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                               const bool pixelState, const EpdFontFamily::Style style, const int scaleNum,
                               const int scaleDen) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) return;

  const EpdFontData* fontData = fontFamily.getData(style);
  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
  if (!bitmap) return;

  const int srcW = glyph->width;
  const int srcH = glyph->height;
  const int dstW = (srcW * scaleNum + scaleDen - 1) / scaleDen;
  const int dstH = (srcH * scaleNum + scaleDen - 1) / scaleDen;
  const int baseX = cursorX + (glyph->left * scaleNum) / scaleDen;
  const int baseY = cursorY - (glyph->top * scaleNum) / scaleDen;
  const bool state = (fontData->is2Bit && renderMode != GfxRenderer::BW) ? false : pixelState;

  if (fontData->is2Bit) {
    // Bilinear-interpolate the raw 4-level coverage (0=white..3=black) instead
    // of nearest-neighbor block-replicating it -- nearest-neighbor reproduces
    // the source's staircase edges at 1.5x scale instead of smoothing them,
    // which read as visibly blocky/low-quality next to a real Mushaf's
    // natively-antialiased calligraphy. Interpolating first, then applying the
    // exact same per-renderMode threshold renderCharImpl uses, moves each
    // destination pixel's effective edge position to sub-source-pixel
    // precision -- the same principle as supersampled antialiasing.
    auto sampleRaw = [&](int sx, int sy) -> int {
      sx = std::min(sx, srcW - 1);
      sy = std::min(sy, srcH - 1);
      const int pos = sy * srcW + sx;
      const uint8_t byte = bitmap[pos >> 2];
      const uint8_t bitIndex = (3 - (pos & 3)) * 2;
      return (byte >> bitIndex) & 0x3;
    };
    for (int dstY = 0; dstY < dstH; dstY++) {
      const int srcYFixed = dstY * scaleDen;  // srcY = srcYFixed / scaleNum
      const int srcY0 = srcYFixed / scaleNum;
      const int fracY = srcYFixed % scaleNum;
      for (int dstX = 0; dstX < dstW; dstX++) {
        const int srcXFixed = dstX * scaleDen;
        const int srcX0 = srcXFixed / scaleNum;
        const int fracX = srcXFixed % scaleNum;
        const int v00 = sampleRaw(srcX0, srcY0);
        const int v10 = sampleRaw(srcX0 + 1, srcY0);
        const int v01 = sampleRaw(srcX0, srcY0 + 1);
        const int v11 = sampleRaw(srcX0 + 1, srcY0 + 1);
        const int top = v00 * (scaleNum - fracX) + v10 * fracX;
        const int bot = v01 * (scaleNum - fracX) + v11 * fracX;
        const int raw = (top * (scaleNum - fracY) + bot * fracY + (scaleNum * scaleNum) / 2) / (scaleNum * scaleNum);
        const uint8_t bmpVal = 3 - static_cast<uint8_t>(raw);
        bool draw;
        if (renderMode == GfxRenderer::BW) {
          draw = bmpVal < 3;
        } else if (renderMode == GfxRenderer::GRAYSCALE_MSB) {
          draw = bmpVal == 1 || bmpVal == 2;
        } else {
          draw = bmpVal == 1;  // GRAYSCALE_LSB
        }
        if (draw) renderer.drawPixel(baseX + dstX, baseY + dstY, state);
      }
    }
  } else {
    // 1-bit source has no graduated coverage to interpolate -- plain
    // nearest-neighbor sampling.
    for (int dstY = 0; dstY < dstH; dstY++) {
      const int srcY = dstY * scaleDen / scaleNum;
      if (srcY >= srcH) continue;
      for (int dstX = 0; dstX < dstW; dstX++) {
        const int srcX = dstX * scaleDen / scaleNum;
        if (srcX >= srcW) continue;
        const int pos = srcY * srcW + srcX;
        const uint8_t byte = bitmap[pos >> 3];
        const uint8_t bitIndex = 7 - (pos & 7);
        if ((byte >> bitIndex) & 1) renderer.drawPixel(baseX + dstX, baseY + dstY, state);
      }
    }
  }
}

template <TextRotation rotation = TextRotation::None>
static void renderCharImpl(const GfxRenderer& renderer, GfxRenderer::RenderMode renderMode,
                           const EpdFontFamily& fontFamily, const uint32_t cp, int cursorX, int cursorY,
                           const bool pixelState, const EpdFontFamily::Style style) {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    LOG_ERR("GFX", "No glyph for codepoint %d", cp);
    return;
  }

  const EpdFontData* fontData = fontFamily.getData(style);
  const bool is2Bit = fontData->is2Bit;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;
  const int top = glyph->top;

  // Tiled-grayscale band culling: if this glyph's physical y-extent is entirely
  // outside the active strip, skip it before the expensive bitmap decode. This
  // is what makes per-band re-rendering cheap. No-op outside strip mode.
  if constexpr (rotation == TextRotation::Rotated90CW) {
    const int ob = cursorX + fontData->ascender - top;
    const int ib = cursorY - left;
    if (!renderer.glyphIntersectsStrip(ob, ib - (width - 1), ob + height - 1, ib)) {
      return;
    }
  } else {
    const int gx0 = cursorX + left;
    const int gy0 = cursorY - top;
    if (!renderer.glyphIntersectsStrip(gx0, gy0, gx0 + width - 1, gy0 + height - 1)) {
      return;
    }
  }

  const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);

  if (bitmap != nullptr) {
    // For Normal:  outer loop advances screenY, inner loop advances screenX
    // For Rotated: outer loop advances screenX, inner loop advances screenY (in reverse)
    int outerBase, innerBase;
    if constexpr (rotation == TextRotation::Rotated90CW) {
      outerBase = cursorX + fontData->ascender - top;  // screenX = outerBase + glyphY
      innerBase = cursorY - left;                      // screenY = innerBase - glyphX
    } else {
      outerBase = cursorY - top;   // screenY = outerBase + glyphY
      innerBase = cursorX + left;  // screenX = innerBase + glyphX
    }

    if (is2Bit) {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 2];
          const uint8_t bit_index = (3 - (pixelPosition & 3)) * 2;
          // the direct bit from the font is 0 -> white, 1 -> light gray, 2 -> dark gray, 3 -> black
          // we swap this to better match the way images and screen think about colors:
          // 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white
          const uint8_t bmpVal = 3 - ((byte >> bit_index) & 0x3);

          if (renderMode == GfxRenderer::BW && bmpVal < 3) {
            // Black (also paints over the grays in BW mode)
            renderer.drawPixel(screenX, screenY, pixelState);
          } else if (renderMode == GfxRenderer::GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
            // Light gray (also mark the MSB if it's going to be a dark gray too)
            // Dedicated X3 gray LUTs now provide proper 4-level gray on both devices
            // We have to flag pixels in reverse for the gray buffers, as 0 leave alone, 1 update
            renderer.drawPixel(screenX, screenY, false);
          } else if (renderMode == GfxRenderer::GRAYSCALE_LSB && bmpVal == 1) {
            // Dark gray
            renderer.drawPixel(screenX, screenY, false);
          }
        }
      }
    } else {
      int pixelPosition = 0;
      for (int glyphY = 0; glyphY < height; glyphY++) {
        const int outerCoord = outerBase + glyphY;
        for (int glyphX = 0; glyphX < width; glyphX++, pixelPosition++) {
          int screenX, screenY;
          if constexpr (rotation == TextRotation::Rotated90CW) {
            screenX = outerCoord;
            screenY = innerBase - glyphX;
          } else {
            screenX = innerBase + glyphX;
            screenY = outerCoord;
          }

          const uint8_t byte = bitmap[pixelPosition >> 3];
          const uint8_t bit_index = 7 - (pixelPosition & 7);

          if ((byte >> bit_index) & 1) {
            renderer.drawPixel(screenX, screenY, pixelState);
          }
        }
      }
    }
  }
}

// IMPORTANT: This function is in critical rendering path and is called for every pixel. Please keep it as simple and
// efficient as possible.
void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  int phyX = 0;
  int phyY = 0;

  // Note: this call should be inlined for better performance
  rotateCoordinates(orientation, x, y, &phyX, &phyY, panelWidth, panelHeight);

  // Bounds checking against runtime panel dimensions
  if (phyX < 0 || phyX >= panelWidth || phyY < 0 || phyY >= panelHeight) {
    LOG_ERR("GFX", "!! Outside range (%d, %d) -> (%d, %d)", x, y, phyX, phyY);
    return;
  }

  // Tiled grayscale: redirect writes to the strip scratch and clip to the
  // current band. Single predictable branch on the hot per-pixel path.
  uint8_t* target = frameBuffer;
  uint32_t rowY = static_cast<uint32_t>(phyY);
  if (_stripActive) {
    if (phyY < _stripY0 || phyY >= _stripY0 + _stripRows) {
      return;  // pixel outside the band currently being rendered
    }
    target = _stripBuf;
    rowY = static_cast<uint32_t>(phyY - _stripY0);
  }

  // Calculate byte position and bit position
  const uint32_t byteIndex = rowY * panelWidthBytes + (phyX / 8);
  const uint8_t bitPosition = 7 - (phyX % 8);  // MSB first

  if (state) {
    target[byteIndex] &= ~(1 << bitPosition);  // Clear bit
  } else {
    target[byteIndex] |= 1 << bitPosition;  // Set bit
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                              const BidiUtils::BidiBaseDir baseDir) const {
  if (text == nullptr || *text == '\0') {
    return 0;
  }

  // Arabic needs contextual shaping, not just bidi reordering -- dispatch before
  // resolveVisualText() so the (already visual-order) shaped codepoints aren't
  // run through MiniBidi's reordering a second time.
  if (ScriptDetector::containsArabic(text)) {
    return getArabicTextWidth(fontId, text, style);
  }

  // Measure with the same font drawText would render with (see resolveTextFontId)
  // so wrapping, truncation and centering of CJK strings stay consistent.
  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return 0;
  }

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);

  // Redirected to the SD fallback: batch-load the string's glyphs so the
  // per-codepoint measurement loop below doesn't fault them in one SD read
  // at a time (#2725).
  if (resolvedFontId != fontId) {
    ensureSdGlyphsResident(resolvedFontId, renderedText, style, true);
  }

  int w = 0, h = 0;
  fontIt->second.getTextDimensions(renderedText, &w, &h, style);
  return w;
}

namespace {
void appendUtf8(std::string& out, const uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Ayah-number marker word: exactly "﴿<arabic-indic digits>﴾" (U+FD3F, digits,
// U+FD3E -- see tools/quran/build_quran_epub.py). Rendered as a code-drawn
// circular rosette outline (see ayahRosetteDiameter below) with the number
// drawn half-scale inside it -- not the U+06DD font glyph the name might
// suggest: real-device testing found a user's selected Arabic font simply
// lacked that one specific decorative codepoint, silently dropping every
// single ayah marker in the entire Quran with no fallback (the old code's
// `font.getGlyph(0x06DD, style) != nullptr` guard skipped the whole branch,
// including the digits, whenever it was missing).
bool parseAyahMarker(const char* text, uint32_t* digits, int& digitCount) {
  // Byte-level match (fixed UTF-8 sequences): U+FD3F = EF B4 BF,
  // U+0660..0669 = D9 A0..A9, U+FD3E = EF B4 BE.
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  if (p[0] != 0xEF || p[1] != 0xB4 || p[2] != 0xBF) return false;
  p += 3;
  digitCount = 0;
  while (p[0] == 0xD9 && p[1] >= 0xA0 && p[1] <= 0xA9) {
    if (digitCount >= 3) return false;
    digits[digitCount++] = 0x0660 + (p[1] - 0xA0);
    p += 2;
  }
  if (digitCount == 0) return false;
  if (p[0] != 0xEF || p[1] != 0xB4 || p[2] != 0xBE) return false;
  return p[3] == '\0';
}

// Surah-number marker word: exactly "<Arabic-Indic digits>" wrapped in two
// Private Use Area sentinels the build tool controls entirely -- see
// tools/quran/build_quran_epub.py -- so this can never collide with real
// book text. Deliberately a DIFFERENT visual style from the ayah marker
// above (a filled black disc with inverted white digits, code-drawn rather
// than a font glyph) so the two are never confused at a glance; this marker
// is meant to stand alone on its own right-aligned line, not inline with
// running text.
bool parseSurahMedallionMarker(const char* text, uint32_t* digits, int& digitCount) {
  // U+E000 = EE 80 80, U+0660..0669 = D9 A0..A9, U+E001 = EE 80 81.
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  if (p[0] != 0xEE || p[1] != 0x80 || p[2] != 0x80) return false;
  p += 3;
  digitCount = 0;
  while (p[0] == 0xD9 && p[1] >= 0xA0 && p[1] <= 0xA9) {
    if (digitCount >= 3) return false;
    digits[digitCount++] = 0x0660 + (p[1] - 0xA0);
    p += 2;
  }
  if (digitCount == 0) return false;
  if (p[0] != 0xEE || p[1] != 0x80 || p[2] != 0x81) return false;
  return p[3] == '\0';
}

// Diameter shared by the render and width-measurement branches below -- they
// must agree exactly, or the ayah marker's own advance width (inline with
// running text, unlike the surah medallion's standalone line) would be
// computed against the wrong shape. Smaller than the surah medallion below
// since this sits inline with body text rather than standing alone.
int ayahRosetteDiameter(const GfxRenderer& renderer, const int arabicFontId) {
  return renderer.getFontAscenderSize(arabicFontId) * 3 / 4;
}

// Diameter shared by the render and width-measurement branches below -- they
// must agree exactly, or the medallion's own layout (a right-aligned line
// containing nothing else) would center/clip against the wrong width.
int surahMedallionDiameter(const GfxRenderer& renderer, const int arabicFontId) {
  return renderer.getFontAscenderSize(arabicFontId);
}

// Surah-name marker: the surah NAME wrapped in U+E002/U+E003 Private Use Area
// sentinels (see tools/quran/build_quran_epub.py) -- another build-tool-controlled
// pair, like the surah-medallion marker above, so it can never collide with real
// book text. Unlike the fixed-digit markers, the wrapped payload is arbitrary-length
// (the name itself), so this returns the inner UTF-8 slice rather than parsed digits.
// Renders as a code-drawn cartouche: a pointed horizontal band styled after the
// mushaf's illuminated surah-heading banner, replacing the plain
// flanked-by-glyph text this line used before.
bool parseCartoucheMarker(const char* text, std::string& inner) {
  // U+E002 = EE 80 82, U+E003 = EE 80 83.
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  if (p[0] != 0xEE || p[1] != 0x80 || p[2] != 0x82) return false;
  const char* innerStart = text + 3;
  const size_t len = strlen(innerStart);
  if (len < 3) return false;
  const auto* tail = reinterpret_cast<const uint8_t*>(innerStart + len - 3);
  if (tail[0] != 0xEE || tail[1] != 0x80 || tail[2] != 0x83) return false;
  inner.assign(innerStart, len - 3);
  return true;
}

// Geometry shared by the render and width-measurement branches below -- must agree
// exactly, like surahMedallionDiameter above. `tip` is the length of the pointed
// cusp at each end, `pad` the gap between the name text and where the flat top/
// bottom edges begin.
struct CartoucheGeometry {
  int height;
  int tip;
  int pad;
  int width;
};
CartoucheGeometry cartoucheGeometryFor(const int ascender, const int innerTextWidth) {
  CartoucheGeometry g;
  g.height = ascender + ascender / 4;
  g.tip = g.height / 2;
  g.pad = g.height / 4;
  g.width = innerTextWidth + 2 * g.pad + 2 * g.tip;
  return g;
}

// ChapterHtmlSlimParser tokenizes on any whitespace byte -- including U+00A0/U+202F
// no-break spaces -- splitting a multi-word name across separate drawArabicText()
// calls before the cartouche sentinels could ever be seen together. The build tool
// replaces spaces inside the name with this PUA sentinel instead (not whitespace to
// that tokenizer), and both branches below render it as a fixed-width gap rather than
// looking it up as a font glyph.
constexpr uint32_t CARTOUCHE_SPACE_CP = 0xE004;
int cartoucheSpaceWidth(const int ascender) { return ascender / 3; }

// Bismillah draw scale -- see renderCharUpscaled's comment for why this exists
// (the baked glyph is already at the format's uint8_t size cap). 3/2 = 1.5x,
// tuned visually against a real Mushaf reference photo's Bismillah-to-ayah-text
// size ratio; a flat 2x was tried first and looked oversized. Shared by the
// measure and render branches below -- must agree exactly, like every other
// marker's shared geometry in this file.
constexpr int BISMILLAH_SCALE_NUM = 3;
constexpr int BISMILLAH_SCALE_DEN = 2;

// Bismillah marker: the real ligature glyph U+FDFD wrapped in U+E005/U+E006 Private
// Use Area sentinels (see tools/quran/build_quran_epub_kfgqpc.py) -- same
// build-tool-controlled convention as the medallion/cartouche markers above. Renders
// as a single glyph sourced from bismillahFontId_ (a tiny dedicated font -- see
// QuranCommon's registration comment in main.cpp), since UthmanicHafs itself has no
// glyph for U+FDFD.
bool parseBismillahMarker(const char* text) {
  // U+E005 = EE 80 85, U+FDFD = EF B7 BD, U+E006 = EE 80 86.
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  if (p[0] != 0xEE || p[1] != 0x80 || p[2] != 0x85) return false;
  p += 3;
  if (p[0] != 0xEF || p[1] != 0xB7 || p[2] != 0xBD) return false;
  p += 3;
  if (p[0] != 0xEE || p[1] != 0x80 || p[2] != 0x86) return false;
  return p[3] == '\0';
}

// The shared ornament glyph baked alongside the 114 per-surah name glyphs (see
// tools/quran/gen_surah_banner_glyphmap.py) -- one codepoint per surah starting at
// this base, then one final codepoint for the ornament shared by all of them.
constexpr uint32_t SURAH_BANNER_NAME_BASE_CP = 0xE010;
constexpr uint32_t SURAH_BANNER_ORNAMENT_CP = 0xE010 + 114;  // 0xE082

// Surah-banner marker: replaces the old medallion+cartouche pair with a single
// header row styled after the Madinah Mushaf's own surah-heading banner -- rule
// lines above/below, ayah-count label on the right, revelation-order label on the
// left, the surah's calligraphic name centered between them (see
// tools/quran/build_quran_epub_kfgqpc.py). Wire format, all build-tool-controlled
// PUA sentinels so it can never collide with real book text:
//   U+E007 <name-glyph codepoint, literal 3-byte UTF-8> <right label text>
//   U+E009 <left label text> U+E008
// The two label strings are plain Arabic text composed by the build script
// (e.g. "آياتها ١٨"), not re-derived here, so this code never needs to know how
// to spell "ayahs"/"revelation order" in Arabic -- same reasoning as the
// cartouche marker passing the surah NAME through verbatim instead of hardcoding
// it. Unlike the fixed-digit markers above, the label payloads are
// arbitrary-length, so this returns UTF-8 slices rather than parsed digits.
bool parseSurahBannerMarker(const char* text, uint32_t& nameGlyphCp, std::string& rightLabel, std::string& leftLabel) {
  // U+E007 = EE 80 87, U+E009 = EE 80 89, U+E008 = EE 80 88.
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  if (p[0] != 0xEE || p[1] != 0x80 || p[2] != 0x87) return false;
  p += 3;
  if ((p[0] & 0xF0) != 0xE0) return false;  // name glyph cp is always a 3-byte UTF-8 sequence
  const uint32_t cp = (static_cast<uint32_t>(p[0] & 0x0F) << 12) | (static_cast<uint32_t>(p[1] & 0x3F) << 6) |
                      static_cast<uint32_t>(p[2] & 0x3F);
  if (cp < SURAH_BANNER_NAME_BASE_CP || cp >= SURAH_BANNER_NAME_BASE_CP + 114) return false;
  nameGlyphCp = cp;
  p += 3;

  const char* rightStart = reinterpret_cast<const char*>(p);
  const char* sep = strstr(rightStart, "\xEE\x80\x89");  // U+E009
  if (!sep) return false;
  rightLabel.assign(rightStart, static_cast<size_t>(sep - rightStart));

  const char* leftStart = sep + 3;
  const size_t leftLen = strlen(leftStart);
  if (leftLen < 3) return false;
  const auto* tail = reinterpret_cast<const uint8_t*>(leftStart + leftLen - 3);
  if (tail[0] != 0xEE || tail[1] != 0x80 || tail[2] != 0x88) return false;  // U+E008
  leftLabel.assign(leftStart, leftLen - 3);
  return true;
}

// The row spans the full screen width minus the hardware-safe margin -- a
// full-bleed header like the Mushaf's own, not a content-sized box (unlike the
// medallion/cartouche/Bismillah markers above, which size to their own tight
// content). This is the ONLY thing that must match between the measure and
// render branches below (per this file's own "must match" convention) --
// vertical layout (row height, label stacking) is render-only, since
// getArabicTextWidth never needs it.
int surahBannerWidth(const int screenWidth) {
  return screenWidth - GfxRenderer::VIEWABLE_MARGIN_LEFT - GfxRenderer::VIEWABLE_MARGIN_RIGHT;
}

// Splits a label ("<word>" + CARTOUCHE_SPACE_CP + "<digits>", see
// parseSurahBannerMarker's comment) into its word and digit parts so they can
// be drawn on two stacked lines -- word above, digits centered below it --
// instead of inline, matching the Mushaf's own header layout.
void splitSurahBannerLabel(const std::string& label, std::string& word, std::string& digits) {
  const char* sep = strstr(label.c_str(), "\xEE\x80\x84");  // U+E004 = CARTOUCHE_SPACE_CP
  if (!sep) {
    word = label;
    digits.clear();
    return;
  }
  word.assign(label.c_str(), static_cast<size_t>(sep - label.c_str()));
  digits.assign(sep + 3);
}
}  // namespace

namespace ArabicTextMarkers {
bool isBismillah(const char* text) { return parseBismillahMarker(text); }
}  // namespace ArabicTextMarkers

int GfxRenderer::getArabicTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style,
                                    const int kashidaExtraPx) const {
  if (text == nullptr || *text == '\0') return 0;

  const auto fontIt = fontMap.find(resolveArabicFontId(fontId));
  if (fontIt == fontMap.end()) {
    // ArabicFontSystem::begin() always sets arabicFontId_ to a valid font (the
    // built-in Noto Sans Arabic, or an SD override) before any activity can render
    // text, so this only fires in the brief pre-begin() window. Matches
    // EpdFont::getGlyph's existing missing-glyph behaviour elsewhere: skip silently
    // rather than fall back to fontId's (Arabic-less) font.
    return 0;
  }
  const auto& font = fontIt->second;

  // Ayah markers lay out at the font glyph's advance when U+06DD is present, or
  // the code-drawn rosette's diameter when it's not -- must match drawArabicText's
  // ayah branch exactly.
  {
    uint32_t digits[3];
    int digitCount = 0;
    if (parseAyahMarker(text, digits, digitCount)) {
      if (const EpdGlyph* rosette = font.getGlyph(0x06DD, style)) {
        return fp4::toPixel(rosette->advanceX);
      }
      return ayahRosetteDiameter(*this, resolveArabicFontId(fontId));
    }
  }

  // Surah medallions lay out at the code-drawn disc's diameter -- must match
  // drawArabicText's medallion branch exactly.
  {
    uint32_t digits[3];
    int digitCount = 0;
    if (parseSurahMedallionMarker(text, digits, digitCount)) {
      return surahMedallionDiameter(*this, resolveArabicFontId(fontId));
    }
  }

  // Surah-name cartouches lay out at the code-drawn banner's full width (padding +
  // pointed tips included) -- must match drawArabicText's cartouche branch exactly.
  {
    std::string inner;
    if (parseCartoucheMarker(text, inner)) {
      const int ascender = getFontAscenderSize(resolveArabicFontId(fontId));
      int innerWidth = 0;
      for (const uint32_t cp :
           ArabicShaper::shapeText(inner.c_str(), [&](uint32_t c) { return font.getGlyph(c, style) != nullptr; })) {
        if (cp == CARTOUCHE_SPACE_CP) {
          innerWidth += cartoucheSpaceWidth(ascender);
          continue;
        }
        const EpdGlyph* glyph = font.getGlyph(cp, style);
        if (glyph) innerWidth += fp4::toPixel(glyph->advanceX);
      }
      return cartoucheGeometryFor(ascender, innerWidth).width;
    }
  }

  // The surah banner lays out at the combined width of both labels plus the
  // calligraphy pair (sourced from surahBannerFontId_, NOT the resolved Arabic
  // font above) -- must match drawArabicText's surah-banner branch exactly.
  {
    uint32_t nameGlyphCp = 0;
    std::string rightLabel, leftLabel;
    if (parseSurahBannerMarker(text, nameGlyphCp, rightLabel, leftLabel)) {
      const auto bannerFontIt = fontMap.find(surahBannerFontId_);
      if (bannerFontIt == fontMap.end()) return 0;
      if (!bannerFontIt->second.getGlyph(nameGlyphCp, style) ||
          !bannerFontIt->second.getGlyph(SURAH_BANNER_ORNAMENT_CP, style)) {
        return 0;
      }
      return surahBannerWidth(getScreenWidth());
    }
  }

  // The Bismillah marker lays out at BISMILLAH_SCALE_NUM/DEN times its
  // dedicated font's own glyph advance -- drawn via renderCharUpscaled (see its
  // comment for why: the baked glyph is already at the uint8_t width cap, so
  // it's scaled up at draw time instead of baked bigger) -- must match
  // drawArabicText's Bismillah branch exactly. Sourced from bismillahFontId_,
  // NOT the resolved Arabic font above (UthmanicHafs lacks U+FDFD entirely).
  if (parseBismillahMarker(text)) {
    const auto bismillahFontIt = fontMap.find(bismillahFontId_);
    if (bismillahFontIt == fontMap.end()) return 0;
    const EpdGlyph* glyph = bismillahFontIt->second.getGlyph(0xFDFD, style);
    return glyph ? fp4::toPixel(glyph->advanceX) * BISMILLAH_SCALE_NUM / BISMILLAH_SCALE_DEN : 0;
  }

  // Resolve the SD-card backing (if any) once. The Arabic shaper probes glyph
  // existence for every character and this loop reads an advance per shaped glyph;
  // for SD fonts, routing both through the RAM interval table / advance table
  // avoids the per-glyph .cpfont open/seek/read that dominated the scan pass.
  const auto arSdIt = sdCardFonts_.find(resolveArabicFontId(fontId));
  SdCardFont* const sdFont = (arSdIt != sdCardFonts_.end()) ? arSdIt->second : nullptr;
  const uint8_t sdStyleIdx = sdFont ? resolveSdCardStyle(*sdFont, style) : 0;
  if (sdFont) {
    // One-shot per style: fill the advance table with the font's whole Arabic
    // coverage in a single sequential SD pass, so a fresh font's first chapter
    // re-layout doesn't refill it through dozens of tiny per-word batches
    // (each its own .cpfont open). No-op after the first call.
    sdFont->prewarmArabicAdvances(sdStyleIdx);
  }

  const auto hasGlyphFn = [&](uint32_t c) {
    return sdFont ? sdFont->hasGlyph(c, sdStyleIdx) : (font.getGlyph(c, style) != nullptr);
  };
  int tatweelPx = 0;
  if (kashidaExtraPx > 0) {
    if (const EpdGlyph* tatweel = font.getGlyph(0x0640, style)) tatweelPx = fp4::toPixel(tatweel->advanceX);
  }
  const auto cps = tatweelPx > 0 ? ArabicShaper::shapeTextWithKashida(text, kashidaExtraPx, tatweelPx, hasGlyphFn)
                                 : ArabicShaper::shapeText(text, hasGlyphFn);
  if (sdFont) {
    // The advance table is prebuilt from RAW text codepoints, but this loop looks
    // up SHAPED presentation forms (U+FExx contextual variants) -- which the
    // prebuild can never contain. Without this top-up every shaped glyph of every
    // word missed the table and fell back to getGlyph's per-glyph on-demand SD
    // load (.cpfont open+seek+read through an 8-slot ring): a section build
    // measuring thousands of words issued tens of thousands of SD reads, turning
    // "indexing" into minutes of dead-input crawl on real hardware. Batch the
    // missing shaped forms into the table instead -- one mostly-sequential SD
    // pass per NEW glyph, amortized across the session (fetch skips
    // already-cached codepoints without touching the SD at all).
    std::string missingUtf8;
    for (const uint32_t cp : cps) {
      if (!utf8IsCombiningMark(cp) && sdFont->getAdvance(cp, sdStyleIdx) == 0 && sdFont->hasGlyph(cp, sdStyleIdx)) {
        appendUtf8(missingUtf8, cp);
      }
    }
    if (!missingUtf8.empty()) {
      sdFont->buildAdvanceTable(missingUtf8.c_str(), static_cast<uint8_t>(1u << sdStyleIdx));
    }
  }
  int width = 0;
  for (const uint32_t cp : cps) {
    // Per-glyph pixel rounding (fp4::toPixel each advance, then sum) is preserved
    // exactly so measured width still matches drawArabicText's per-glyph pen
    // advance. Only the SOURCE of the advance changes for SD fonts: the advance
    // table (RAM) first, falling back to getGlyph only on a genuine miss.
    if (sdFont) {
      int32_t advFP = sdFont->getAdvance(cp, sdStyleIdx);
      if (advFP == 0 && !utf8IsCombiningMark(cp)) {
        const EpdGlyph* glyph = font.getGlyph(cp, style);
        advFP = glyph ? glyph->advanceX : 0;
      }
      width += fp4::toPixel(advFP);
    } else {
      const EpdGlyph* glyph = font.getGlyph(cp, style);
      if (glyph) {
        width += fp4::toPixel(glyph->advanceX);
      }
    }
  }
  return width;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style, baseDir)) / 2;
  drawText(fontId, x, y, text, black, style, baseDir);
}

int GfxRenderer::drawCenteredTextWrapped(const int fontId, const int y, const int maxWidth, const char* text,
                                         const int maxLines, const bool black, const EpdFontFamily::Style style) const {
  return layoutCenteredTextWrapped(fontId, y, maxWidth, text, maxLines, black, style, /*draw=*/true);
}

int GfxRenderer::measureWrappedTextHeight(const int fontId, const int maxWidth, const char* text, const int maxLines,
                                          const EpdFontFamily::Style style) const {
  return layoutCenteredTextWrapped(fontId, /*y=*/0, maxWidth, text, maxLines, /*black=*/true, style, /*draw=*/false);
}

int GfxRenderer::layoutCenteredTextWrapped(const int fontId, const int y, const int maxWidth, const char* text,
                                           const int maxLines, const bool black, const EpdFontFamily::Style style,
                                           const bool draw) const {
  if (text == nullptr || *text == '\0' || maxWidth <= 0 || maxLines <= 0) return 0;

  const int lineHeight = getLineHeight(fontId);
  std::string_view remaining{text};
  // Reused across every line so a wrapped message costs one buffer, not one per row.
  std::string line;
  // Counts laid-out rows, painted or not -- the measure path walks the same loop.
  int linesDrawn = 0;

  while (linesDrawn < maxLines) {
    const auto firstNonSpace = remaining.find_first_not_of(' ');
    if (firstNonSpace == std::string_view::npos) break;  // only trailing spaces left
    remaining.remove_prefix(firstNonSpace);

    // Final permitted row: hand the whole remainder to truncatedText, which either fits it
    // or ellipsizes. Keeps "there is more text" visible rather than dropping it silently.
    if (linesDrawn == maxLines - 1) {
      line = truncatedText(fontId, std::string(remaining).c_str(), maxWidth, style);
      if (draw) drawCenteredText(fontId, y + linesDrawn * lineHeight, line.c_str(), black, style);
      return ++linesDrawn * lineHeight;
    }

    // Extend one word at a time and keep the longest prefix that still fits.
    size_t bestEnd = 0;
    size_t scan = 0;
    while (true) {
      const size_t nextSpace = remaining.find(' ', scan);
      const size_t end = nextSpace == std::string_view::npos ? remaining.size() : nextSpace;
      line.assign(remaining.substr(0, end));
      if (getTextWidth(fontId, line.c_str(), style) > maxWidth) break;
      bestEnd = end;
      if (nextSpace == std::string_view::npos) break;
      scan = nextSpace + 1;
    }

    if (bestEnd == 0) {
      // First word alone overruns the width -- ellipsize it so the row is still legible.
      const size_t firstSpace = remaining.find(' ');
      bestEnd = firstSpace == std::string_view::npos ? remaining.size() : firstSpace;
      line = truncatedText(fontId, std::string(remaining.substr(0, bestEnd)).c_str(), maxWidth, style);
    } else {
      line.assign(remaining.substr(0, bestEnd));
    }

    if (draw) drawCenteredText(fontId, y + linesDrawn * lineHeight, line.c_str(), black, style);
    ++linesDrawn;
    remaining.remove_prefix(bestEnd);
  }

  return linesDrawn * lineHeight;
}

void GfxRenderer::drawTextInWidth(const int fontId, const int x, const int y, const int width, const char* text,
                                  const bool black, const EpdFontFamily::Style style) const {
  if (text == nullptr || *text == '\0') return;

  if (ScriptDetector::containsArabic(text)) {
    const int textWidth = getTextWidth(fontId, text, style);
    const int rightAlignedX = x + width - textWidth;
    drawText(fontId, rightAlignedX, y, text, black, style);
    return;
  }

  drawText(fontId, x, y, text, black, style);
}

void GfxRenderer::drawArabicText(const int fontId, const int x, const int y, const char* text, const bool black,
                                 const EpdFontFamily::Style style, const int kashidaExtraPx) const {
  if (text == nullptr || *text == '\0') return;

  const int resolvedArabicFontId = resolveArabicFontId(fontId);
  const auto fontIt = fontMap.find(resolvedArabicFontId);

  // Diagnostics only: real-device logs showed the Arabic prewarm scan capturing 0
  // bytes on every single Quran page turn, with no way to tell whether this function
  // is even being entered during the scan pass, or entered but bailing out below
  // before ever reaching recordArabicText(). Counted unconditionally, before the
  // early return, so it can't itself be skipped by whatever's causing the mismatch.
  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->noteArabicScanEntry(fontIt != fontMap.end(), resolvedArabicFontId);
  }

  if (fontIt == fontMap.end()) {
    // No Arabic font loaded -- nothing we can draw with. Matches the width path above.
    return;
  }
  const auto& font = fontIt->second;

  // SD-card backing (if any) for the resolved Arabic font, so glyph-existence
  // probes during shaping answer from the RAM interval table instead of a
  // .cpfont open/seek/read per character. This recording/shaping runs on every
  // scan-pass page turn; on SD-Arabic fonts the getGlyph-based existence checks
  // were the dominant cost of a multi-second turn (see getArabicTextWidth).
  const auto arSdIt = sdCardFonts_.find(resolvedArabicFontId);
  SdCardFont* const arSdFont = (arSdIt != sdCardFonts_.end()) ? arSdIt->second : nullptr;
  const uint8_t arSdStyleIdx = arSdFont ? resolveSdCardStyle(*arSdFont, style) : 0;
  const auto fontHasGlyph = [&](uint32_t c) {
    return arSdFont ? arSdFont->hasGlyph(c, arSdStyleIdx) : (font.getGlyph(c, style) != nullptr);
  };

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    // Record the codepoints this call will ACTUALLY draw -- not just the raw input
    // shaped as plain text. The marker branches below never draw their literal
    // syntax (brackets, PUA sentinels): an ayah marker draws U+06DD + digit glyphs
    // when the font has that glyph, or just digit glyphs sourced from a different
    // font (arabicDigitFallbackFontId_, nothing of resolvedArabicFontId's to
    // prewarm) when it doesn't -- must check the exact same font.getGlyph(0x06DD,
    // ...) condition as the render branch below so the codepoints recorded here
    // match what actually gets drawn. A surah medallion draws only digit glyphs,
    // a cartouche draws its inner text glyphs. Recording the wrong codepoints here
    // means the real render pass's marker glyphs never land in the prewarmed page
    // slot and always fall through to FontDecompressor's slow per-glyph hot-group
    // fallback -- confirmed on a real device: 432 of 514 glyph draws missing the
    // cache on a single page, ~13s spent re-decompressing groups from scratch for
    // glyphs that were "prewarmed" under the wrong codepoints entirely. Must stay
    // in exact sync with the render branches below (same reasoning as
    // getArabicTextWidth's marker branches vs. render).
    // Bismillah draws a single glyph from bismillahFontId_, a DIFFERENT font than
    // resolvedArabicFontId -- must be recorded against its own font id, or the
    // real render pass's glyph never lands in the prewarmed slot for that font
    // (same class of bug already fixed once this session for the other markers).
    if (parseBismillahMarker(text)) {
      std::string bismillahShaped;
      appendUtf8(bismillahShaped, 0xFDFD);
      fontCacheManager_->recordArabicText(bismillahShaped.c_str(), bismillahFontId_);
      return;
    }

    // Surah banner draws two label runs from resolvedArabicFontId AND a name+
    // ornament glyph pair from surahBannerFontId_, a DIFFERENT font -- same
    // multi-font recording need as Bismillah above, just split across both fonts
    // instead of one.
    {
      uint32_t bannerNameCp = 0;
      std::string rightLabel, leftLabel;
      if (parseSurahBannerMarker(text, bannerNameCp, rightLabel, leftLabel)) {
        std::string bannerGlyphs;
        appendUtf8(bannerGlyphs, bannerNameCp);
        appendUtf8(bannerGlyphs, SURAH_BANNER_ORNAMENT_CP);
        fontCacheManager_->recordArabicText(bannerGlyphs.c_str(), surahBannerFontId_);

        // Both labels (word + digits) come from surahBannerLabelFontId_, a
        // DIFFERENT font than resolvedArabicFontId (see its own comment) --
        // must record against it, same reasoning as bannerGlyphs above.
        const auto labelFontIt = fontMap.find(surahBannerLabelFontId_);
        const EpdFontFamily& labelFont = labelFontIt != fontMap.end() ? labelFontIt->second : font;
        std::string labelShaped;
        for (const std::string* label : {&rightLabel, &leftLabel}) {
          for (const uint32_t cp : ArabicShaper::shapeText(
                   label->c_str(), [&](uint32_t c) { return labelFont.getGlyph(c, style) != nullptr; })) {
            if (cp != CARTOUCHE_SPACE_CP) appendUtf8(labelShaped, cp);
          }
        }
        fontCacheManager_->recordArabicText(labelShaped.c_str(), surahBannerLabelFontId_);
        return;
      }
    }

    std::string shaped;
    shaped.reserve(64);
    uint32_t digits[3];
    int digitCount = 0;
    std::string cartoucheInner;
    if (parseAyahMarker(text, digits, digitCount)) {
      if (font.getGlyph(0x06DD, style) != nullptr) {
        appendUtf8(shaped, 0x06DD);
        // Digits always come from arabicDigitFallbackFontId_ now, even when the
        // rosette glyph itself comes from the reading font -- see the render
        // branch's comment for why. Record them against that font, not
        // resolvedArabicFontId, same "must stay in sync" rule as everywhere else
        // in this scan.
        std::string digitsShaped;
        for (int i = 0; i < digitCount; i++) appendUtf8(digitsShaped, digits[i]);
        fontCacheManager_->recordArabicText(digitsShaped.c_str(), arabicDigitFallbackFontId_);
      }
      // else: digits come from arabicDigitFallbackFontId_, nothing to record here.
    } else if (parseSurahMedallionMarker(text, digits, digitCount)) {
      for (int i = 0; i < digitCount; i++) appendUtf8(shaped, digits[i]);
    } else if (parseCartoucheMarker(text, cartoucheInner)) {
      for (const uint32_t cp : ArabicShaper::shapeText(cartoucheInner.c_str(), fontHasGlyph)) {
        if (cp != CARTOUCHE_SPACE_CP) appendUtf8(shaped, cp);
      }
    } else {
      for (const uint32_t cp : ArabicShaper::shapeText(text, fontHasGlyph)) appendUtf8(shaped, cp);
    }
    fontCacheManager_->recordArabicText(shaped.c_str(), resolvedArabicFontId);
    return;
  }

  // UI-font mappings anchor on the requested Latin font's baseline so Arabic labels
  // fit the fixed Latin-sized UI geometry and align with adjacent Latin text; reading
  // mappings keep the Arabic font's own (taller) ascender -- see
  // arabicBaselineMatchFontIds_ in the header for the full reasoning.
  const bool matchLatinBaseline = arabicBaselineMatchFontIds_.count(fontId) > 0 && fontMap.count(fontId) > 0;
  const int yPos = y + getFontAscenderSize(matchLatinBaseline ? fontId : resolvedArabicFontId);

  // Ayah markers prefer the font's own END OF AYAH ornament glyph (U+06DD): a
  // properly designed Quranic font's rosette looks much better than any code-drawn
  // substitute. Fixing a GPOS mark-repositioning bug in a previous built-in reading
  // face's compiled font data (see parseAyahMarker's history above) confirmed this
  // glyph-based rosette IS the "correct"/expected look, not the plain circle outline
  // this code drew as a stopgap while U+06DD appeared broken. Falls back to a
  // code-drawn circle outline only if the glyph is genuinely absent, so the marker
  // still isn't silently invisible for some other font. Width must match
  // getArabicTextWidth's marker branch exactly (layout vs render).
  {
    uint32_t digits[3];
    int digitCount = 0;
    if (parseAyahMarker(text, digits, digitCount)) {
      if (const EpdGlyph* rosette = font.getGlyph(0x06DD, style)) {
        renderCharImpl<TextRotation::None>(*this, renderMode, font, 0x06DD, x, yPos, black, style);
        // Digits always come from arabicDigitFallbackFontId_ (a known-complete,
        // properly-sized built-in font), never the reading font -- some reading
        // fonts' own Arabic-Indic digit glyphs are tiny by design (e.g.
        // UthmanicHafs's are only ~4-7px at 18pt, next to a ~26px letter), so
        // drawing them here -- already at half scale via renderCharScaled below --
        // produced a barely-visible number even though the rosette itself was
        // correctly sized. Mirrors the fallback branch below, which already got
        // this right for fonts lacking U+06DD.
        const auto digitFontIt = fontMap.find(arabicDigitFallbackFontId_);
        const EpdFontFamily& digitFont = digitFontIt != fontMap.end() ? digitFontIt->second : font;
        int digitsW = 0;
        const int rosetteCenter = rosette->top - rosette->height / 2;  // ink centre above baseline
        for (int i = 0; i < digitCount; i++) {
          const EpdGlyph* d = digitFont.getGlyph(digits[i], style);
          if (d) digitsW += fp4::toPixel(d->advanceX) / 2;
        }
        int dx = x + rosette->left + (std::max(0, rosette->width - digitsW)) / 2;
        for (int i = 0; i < digitCount; i++) {
          const EpdGlyph* d = digitFont.getGlyph(digits[i], style);
          if (!d) continue;
          const int digitCenter = (d->top - d->height / 2) / 2;
          renderCharScaled(*this, renderMode, digitFont, digits[i], dx, yPos - rosetteCenter + digitCenter, black,
                           style);
          dx += fp4::toPixel(d->advanceX) / 2;
        }
        return;
      }

      // Fallback: no U+06DD glyph in this font. Digit glyphs still come from
      // arabicDigitFallbackFontId_ (a known-complete built-in font) rather than
      // the active reading font, which is presumably the one missing glyphs here.
      const auto digitFontIt = fontMap.find(arabicDigitFallbackFontId_);
      const EpdFontFamily& digitFont = digitFontIt != fontMap.end() ? digitFontIt->second : font;

      const int d = ayahRosetteDiameter(*this, resolvedArabicFontId);
      const int top = yPos - d;
      constexpr int kLineWidth = 2;
      drawRoundedRect(x, top, d, d, kLineWidth, d / 2, true);

      // Digits in visual order (multi-digit numbers read left-to-right), centered
      // inside the outline. digitY is 3/4 of the way down the circle's OWN
      // diameter, not a fixed ascender-based offset -- must stay proportional to
      // d so it still centers correctly if ayahRosetteDiameter's scale factor
      // ever changes.
      int digitsW = 0;
      for (int i = 0; i < digitCount; i++) {
        const EpdGlyph* g = digitFont.getGlyph(digits[i], style);
        if (g) digitsW += fp4::toPixel(g->advanceX) / 2;
      }
      int dx = x + std::max(0, d - digitsW) / 2;
      const int digitY = top + d * 3 / 4;
      for (int i = 0; i < digitCount; i++) {
        const EpdGlyph* g = digitFont.getGlyph(digits[i], style);
        if (!g) continue;
        renderCharScaled(*this, renderMode, digitFont, digits[i], dx, digitY, black, style);
        dx += fp4::toPixel(g->advanceX) / 2;
      }
      return;
    }
  }

  // Surah medallions: a code-drawn filled disc (never a font glyph, so it
  // can't be confused with the ayah rosette above) with the surah number in
  // inverted white digits at half scale. Diameter must match
  // getArabicTextWidth's marker branch exactly (layout vs render). Meant to
  // stand alone on its own right-aligned line (see
  // tools/quran/build_quran_epub.py) -- always drawn black-on-white
  // regardless of the caller's `black`, since the visual point is the
  // stamp-like contrast, not matching surrounding (nonexistent) text.
  {
    uint32_t digits[3];
    int digitCount = 0;
    if (parseSurahMedallionMarker(text, digits, digitCount)) {
      const int d = surahMedallionDiameter(*this, resolvedArabicFontId);
      const int top = yPos - d;
      fillRoundedRect(x, top, d, d, d / 2, Color::Black);

      int digitsW = 0;
      for (int i = 0; i < digitCount; i++) {
        const EpdGlyph* g = font.getGlyph(digits[i], style);
        if (g) digitsW += fp4::toPixel(g->advanceX) / 2;
      }
      int dx = x + std::max(0, d - digitsW) / 2;
      const int digitY = top + d / 2 + getFontAscenderSize(resolvedArabicFontId) / 4;
      for (int i = 0; i < digitCount; i++) {
        const EpdGlyph* g = font.getGlyph(digits[i], style);
        if (!g) continue;
        renderCharScaled(*this, renderMode, font, digits[i], dx, digitY, /*pixelState=*/false, style);
        dx += fp4::toPixel(g->advanceX) / 2;
      }
      return;
    }
  }

  // Surah-name cartouches: a code-drawn pointed banner (six-line hexagon outline,
  // never a font glyph or image) with the surah name centered inside, styled after
  // the mushaf's illuminated surah-heading banner. Geometry must match
  // getArabicTextWidth's cartouche branch exactly (layout vs render).
  {
    std::string inner;
    if (parseCartoucheMarker(text, inner)) {
      const int ascender = getFontAscenderSize(resolvedArabicFontId);
      const auto hasGlyph = [&](uint32_t c) { return font.getGlyph(c, style) != nullptr; };
      int innerWidth = 0;
      for (const uint32_t cp : ArabicShaper::shapeText(inner.c_str(), hasGlyph)) {
        if (cp == CARTOUCHE_SPACE_CP) {
          innerWidth += cartoucheSpaceWidth(ascender);
          continue;
        }
        const EpdGlyph* glyph = font.getGlyph(cp, style);
        if (glyph) innerWidth += fp4::toPixel(glyph->advanceX);
      }
      const CartoucheGeometry g = cartoucheGeometryFor(ascender, innerWidth);
      const int top = yPos - g.height + g.height / 6;
      const int midY = top + g.height / 2;
      constexpr int kLineWidth = 2;
      // Six-point outline: flat top/bottom edges with a pointed cusp at each end.
      drawLine(x + g.tip, top, x + g.width - g.tip, top, kLineWidth, true);
      drawLine(x + g.width - g.tip, top, x + g.width, midY, kLineWidth, true);
      drawLine(x + g.width, midY, x + g.width - g.tip, top + g.height, kLineWidth, true);
      drawLine(x + g.width - g.tip, top + g.height, x + g.tip, top + g.height, kLineWidth, true);
      drawLine(x + g.tip, top + g.height, x, midY, kLineWidth, true);
      drawLine(x, midY, x + g.tip, top, kLineWidth, true);

      int cursorX = x + (g.width - innerWidth) / 2;
      for (const uint32_t cp : ArabicShaper::shapeText(inner.c_str(), hasGlyph)) {
        if (cp == CARTOUCHE_SPACE_CP) {
          cursorX += cartoucheSpaceWidth(ascender);
          continue;
        }
        const EpdGlyph* glyph = font.getGlyph(cp, style);
        if (!glyph) continue;
        renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, cursorX, yPos, black, style);
        cursorX += fp4::toPixel(glyph->advanceX);
      }
      return;
    }
  }

  // Surah banner: rule lines above/below a row containing the ayah-count label
  // (right, word over its digits), the calligraphic surah name + shared
  // ornament glyph (center, sourced from surahBannerFontId_), and the
  // revelation-order label (left, word over its digits) -- see the reference
  // Mushaf photo this layout is benchmarked against. Width must match
  // getArabicTextWidth's surah-banner branch exactly (layout vs render);
  // vertical layout below is render-only.
  {
    uint32_t nameGlyphCp = 0;
    std::string rightLabel, leftLabel;
    if (parseSurahBannerMarker(text, nameGlyphCp, rightLabel, leftLabel)) {
      const auto bannerFontIt = fontMap.find(surahBannerFontId_);
      if (bannerFontIt == fontMap.end()) return;
      const EpdFontFamily& bannerFont = bannerFontIt->second;
      const EpdGlyph* nameGlyph = bannerFont.getGlyph(nameGlyphCp, style);
      const EpdGlyph* ornamentGlyph = bannerFont.getGlyph(SURAH_BANNER_ORNAMENT_CP, style);
      if (!nameGlyph || !ornamentGlyph) return;
      const auto labelFontIt = fontMap.find(surahBannerLabelFontId_);
      const EpdFontFamily& labelFont = labelFontIt != fontMap.end() ? labelFontIt->second : font;
      const auto hasLabelGlyph = [&](uint32_t c) { return labelFont.getGlyph(c, style) != nullptr; };
      const int width = surahBannerWidth(getScreenWidth());

      // Row height: symmetric padding above/below the calligraphy's own ink
      // bounding box -- glyph->top is ascent above baseline, glyph->height
      // minus that is descent below it. The labels are drawn from a genuinely
      // small dedicated font (surahBannerLabelFontId_, see its own comment),
      // sized to comfortably fit inside this calligraphy-only height.
      const int calligraphyAscent = std::max(nameGlyph->top, ornamentGlyph->top);
      const int calligraphyDescent =
          std::max({nameGlyph->height - nameGlyph->top, ornamentGlyph->height - ornamentGlyph->top, 0});
      const int labelAscender = getFontAscenderSize(surahBannerLabelFontId_);
      const int pad = calligraphyAscent / 4;
      const int top = yPos - calligraphyAscent - pad;
      const int bottom = yPos + calligraphyDescent + pad;
      constexpr int kLineWidth = 1;
      drawLine(x, top, x + width, top, kLineWidth, true);
      drawLine(x, bottom, x + width, bottom, kLineWidth, true);

      // Label word's top aligns with the calligraphy's own top; digit's
      // bottom aligns with the calligraphy's own bottom (digits are assumed
      // to have ~zero descent, true for every built-in Arabic-Indic digit set).
      const int wordY = (yPos - calligraphyAscent) + labelAscender;
      const int digitY = yPos + calligraphyDescent;
      // Inset from the rule lines' own full-bleed edges so the labels don't sit
      // flush against the physical screen edge -- rough approximation of the
      // reading column's own margin (not directly available here; the real
      // value is a user-configurable Activity-level setting).
      const int labelInset = labelAscender;

      auto drawStackedLabel = [&](const std::string& label, bool flushRight) {
        std::string word, digits;
        splitSurahBannerLabel(label, word, digits);
        int wordWidth = 0;
        for (const uint32_t cp : ArabicShaper::shapeText(word.c_str(), hasLabelGlyph)) {
          const EpdGlyph* glyph = labelFont.getGlyph(cp, style);
          if (glyph) wordWidth += fp4::toPixel(glyph->advanceX);
        }
        const int wordX = flushRight ? x + width - labelInset - wordWidth : x + labelInset;
        int cursorX = wordX;
        for (const uint32_t cp : ArabicShaper::shapeText(word.c_str(), hasLabelGlyph)) {
          const EpdGlyph* glyph = labelFont.getGlyph(cp, style);
          if (!glyph) continue;
          renderCharImpl<TextRotation::None>(*this, renderMode, labelFont, cp, cursorX, wordY, black, style);
          cursorX += fp4::toPixel(glyph->advanceX);
        }
        if (digits.empty()) return;
        int digitWidth = 0;
        for (const uint32_t cp : ArabicShaper::shapeText(digits.c_str(), hasLabelGlyph)) {
          const EpdGlyph* glyph = labelFont.getGlyph(cp, style);
          if (glyph) digitWidth += fp4::toPixel(glyph->advanceX);
        }
        cursorX = wordX + (wordWidth - digitWidth) / 2;
        for (const uint32_t cp : ArabicShaper::shapeText(digits.c_str(), hasLabelGlyph)) {
          const EpdGlyph* glyph = labelFont.getGlyph(cp, style);
          if (!glyph) continue;
          renderCharImpl<TextRotation::None>(*this, renderMode, labelFont, cp, cursorX, digitY, black, style);
          cursorX += fp4::toPixel(glyph->advanceX);
        }
      };
      drawStackedLabel(rightLabel, /*flushRight=*/true);
      drawStackedLabel(leftLabel, /*flushRight=*/false);

      // Calligraphy pair: name glyph then the shared ornament, centered in the
      // full row width, sharing the same baseline (yPos) as the labels.
      const int calligraphyWidth = fp4::toPixel(nameGlyph->advanceX) + fp4::toPixel(ornamentGlyph->advanceX);
      int cursorX = x + (width - calligraphyWidth) / 2;
      renderCharImpl<TextRotation::None>(*this, renderMode, bannerFont, nameGlyphCp, cursorX, yPos, black, style);
      cursorX += fp4::toPixel(nameGlyph->advanceX);
      renderCharImpl<TextRotation::None>(*this, renderMode, bannerFont, SURAH_BANNER_ORNAMENT_CP, cursorX, yPos, black,
                                         style);
      return;
    }
  }

  // Bismillah: a single glyph (the real U+FDFD ligature) sourced from
  // bismillahFontId_, NOT the resolved Arabic font above -- UthmanicHafs itself
  // has no glyph for U+FDFD. Drawn at BISMILLAH_SCALE_NUM/DEN via
  // renderCharUpscaled (see its comment) since the baked glyph is already at
  // the format's size cap. Width must match getArabicTextWidth's Bismillah
  // branch exactly (layout vs render).
  if (parseBismillahMarker(text)) {
    const auto bismillahFontIt = fontMap.find(bismillahFontId_);
    if (bismillahFontIt == fontMap.end()) return;
    if (bismillahFontIt->second.getGlyph(0xFDFD, style) == nullptr) return;
    renderCharUpscaled(*this, renderMode, bismillahFontIt->second, 0xFDFD, x, yPos, black, style, BISMILLAH_SCALE_NUM,
                       BISMILLAH_SCALE_DEN);
    return;
  }

  {
    const auto hasGlyphFn = [&](uint32_t c) { return font.getGlyph(c, style) != nullptr; };
    int tatweelPx = 0;
    if (kashidaExtraPx > 0) {
      if (const EpdGlyph* tatweel = font.getGlyph(0x0640, style)) tatweelPx = fp4::toPixel(tatweel->advanceX);
    }
    const auto cps = tatweelPx > 0 ? ArabicShaper::shapeTextWithKashida(text, kashidaExtraPx, tatweelPx, hasGlyphFn)
                                   : ArabicShaper::shapeText(text, hasGlyphFn);
    int cursorX = x;
    // Tracks the most recently drawn NON-mark glyph, as a fallback anchor for a
    // diacritic whose forward lookahead (below) finds no base -- e.g. a mark that
    // ends up as the very last codepoint in this call's visual order, with no
    // following glyph at all (can happen at a word/line boundary depending on how
    // upstream tokenization split the text). Mirrors the Latin/Hebrew combining-mark
    // path elsewhere in this file, which always centers backward over the last-drawn
    // base for exactly this reason.
    int lastBaseCursorX = cursorX;
    int lastBaseLeft = 0;
    int lastBaseWidth = 0;
    bool haveLastBase = false;
    for (size_t i = 0; i < cps.size(); i++) {
      const uint32_t cp = cps[i];
      const EpdGlyph* glyph = font.getGlyph(cp, style);
      if (!glyph) continue;

      // Diacritics carry zero advance and only a small natural bitmap_left -- true
      // horizontal centering depends on the base letter's width, which varies letter
      // to letter and can't be baked per-mark at font-conversion time. RTL visual
      // reordering places each mark's base letter immediately AFTER it in draw order
      // (skipping over any other marks stacked on the same base), the mirror image of
      // the Latin/Hebrew combining-mark path elsewhere in this file (which centers
      // over the PREVIOUS glyph, since there the base is drawn first). Without this
      // lookahead, marks render at the joining gap before the next letter instead of
      // over their own base -- tashkeel trailing after the word instead of on it.
      const bool isMark = ArabicShaper::isArabicDiacritic(cp);
      int drawX = cursorX;
      if (isMark) {
        size_t baseIdx = i + 1;
        while (baseIdx < cps.size() && ArabicShaper::isArabicDiacritic(cps[baseIdx])) baseIdx++;
        const combiningMark::Anchor anchor = combiningMark::anchorFor(cp);
        if (baseIdx < cps.size()) {
          if (const EpdGlyph* baseGlyph = font.getGlyph(cps[baseIdx], style)) {
            drawX = combiningMark::anchorOver(anchor, cursorX, baseGlyph->left, baseGlyph->width, glyph->left,
                                              glyph->width);
          }
        } else if (haveLastBase) {
          // No base found ahead (this mark is the last glyph in the call) -- fall
          // back to centering over the last-drawn base instead of the default,
          // uncentered cursorX.
          drawX = combiningMark::anchorOver(anchor, lastBaseCursorX, lastBaseLeft, lastBaseWidth, glyph->left,
                                            glyph->width);
        }
      }

      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, drawX, yPos, black, style);
      if (!isMark) {
        lastBaseCursorX = cursorX;
        lastBaseLeft = glyph->left;
        lastBaseWidth = glyph->width;
        haveLastBase = true;
      }
      cursorX += fp4::toPixel(glyph->advanceX);
    }
  }
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style, const BidiUtils::BidiBaseDir baseDir,
                           const int kashidaExtraPx) const {
  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (ScriptDetector::containsArabic(text)) {
    drawArabicText(fontId, x, y, text, black, style, kashidaExtraPx);
    return;
  }

  // Route CJK-bearing strings to the fallback font when the requested font
  // lacks the glyphs (e.g. Chinese book titles drawn with a Latin UI font).
  const int resolvedFontId = resolveTextFontId(fontId, text, style);

  std::string visual;
  const char* renderedText = resolveVisualText(text, visual, baseDir);

  const int yPos = y + getFontAscenderSize(resolvedFontId);
  int lastBaseX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  if (fontCacheManager_ && fontCacheManager_->isScanning()) {
    fontCacheManager_->recordText(renderedText, resolvedFontId, style);
    return;
  }

  // Redirected to the SD fallback: batch-load the string's glyphs so the draw
  // loop below doesn't fault them in one SD read at a time (#2725).
  if (resolvedFontId != fontId) {
    ensureSdGlyphsResident(resolvedFontId, renderedText, style, false);
  }

  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return;
  }
  const auto& font = fontIt->second;

  const char* textCursor = renderedText;
  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&textCursor)))) {
    // RTL vowel marks (Hebrew niqqud, Arabic harakat) ride the combining-mark
    // path: zero-advance overlays on the preceding base glyph (applyBidiVisual
    // emits base-then-marks per UAX#9 L3). anchorFor pins position-sensitive
    // niqqud (dagesh, shin/sin dots, holam) to their spot on the base; other
    // marks stay centered, raised above the base or (kasra) at their
    // font-native position. Fonts without their glyphs — the built-ins — miss
    // the getGlyph lookup and skip them, as before. (Arabic itself is
    // dispatched to drawArabicText/ArabicShaper above and never reaches this
    // loop; isTransparentMark's Arabic-harakat coverage is inert here.)
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int raiseBy =
          combiningMark::raiseAboveBase(anchor, combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = combiningMark::anchorOver(anchor, lastBaseX, lastBaseLeft, lastBaseWidth,
                                                       combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, combiningX, yPos - raiseBy, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, textCursor, style);

    // Differential rounding: snap (previous advance + current kern) as one unit so
    // identical character pairs always produce the same pixel step regardless of
    // where they fall on the line.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);

    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;  // 12.4 fixed-point

    const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    if (isSupSub) {
      // Halve the advance so the cursor advances by the same amount the scaled glyph
      // actually occupies, keeping spacing correct without needing a separate smaller font.
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    }

    if (isSupSub) {
      // yPos already carries the vertical offset applied by TextBlock::render().
      renderCharScaled(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
    } else {
      renderCharImpl<TextRotation::None>(*this, renderMode, font, cp, lastBaseX, yPos, black, style);
    }
    prevCp = cp;
  }
}

namespace {
const char* resolveVisualText(const char* text, std::string& visualBuffer, const BidiUtils::BidiBaseDir baseDir) {
  if (!text || *text == '\0') return text;

  if (baseDir != BidiUtils::BidiBaseDir::RTL) {
    // Byte-level scan: skip BiDi when no RTL script lead bytes are present.
    // Hebrew UTF-8 lead bytes: 0xD6-0xD7; Arabic/Syriac: 0xD8-0xDB.
    // This covers all RTL content without false negatives and avoids triggering
    // the full UAX#9 algorithm for Latin-extended, em-dashes, accented text, etc.
    bool hasRtlBytes = false;
    for (const unsigned char* q = reinterpret_cast<const unsigned char*>(text); *q; ++q) {
      if (*q >= 0xD6 && *q <= 0xDB) {
        hasRtlBytes = true;
        break;
      }
    }
    if (!hasRtlBytes) return text;
  }

  if (BidiUtils::applyBidiVisual(text, visualBuffer, static_cast<int>(baseDir)) && !visualBuffer.empty()) {
    return visualBuffer.c_str();
  }
  return text;
}
}  // namespace

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, state);
    }
  } else if (y1 == y2) {
    if (x2 < x1) {
      std::swap(x1, x2);
    }
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, state);
    }
  } else {
    // Bresenham's line algorithm — integer arithmetic only
    int dx = x2 - x1;
    int dy = y2 - y1;
    int sx = (dx > 0) ? 1 : -1;
    int sy = (dy > 0) ? 1 : -1;
    dx = sx * dx;  // abs
    dy = sy * dy;  // abs

    int err = dx - dy;
    while (true) {
      drawPixel(x1, y1, state);
      if (x1 == x2 && y1 == y2) break;
      int e2 = 2 * err;
      if (e2 > -dy) {
        err -= dy;
        x1 += sx;
      }
      if (e2 < dx) {
        err += dx;
        y1 += sy;
      }
    }
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const int lineWidth, const bool state) const {
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x1, y1 + i, x2, y2 + i, state);
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

// Border is inside the rectangle
void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const int lineWidth,
                           const bool state) const {
  // Keep the border inside [x, x+width) like the thin overload: the previous
  // right/bottom edges at x+width / y+height sat one pixel outside the rect,
  // so stroked boxes looked shifted against fills computed from the rect.
  for (int i = 0; i < lineWidth; i++) {
    drawLine(x + i, y + i, x + width - 1 - i, y + i, state);
    drawLine(x + width - 1 - i, y + i, x + width - 1 - i, y + height - 1 - i, state);
    drawLine(x + width - 1 - i, y + height - 1 - i, x + i, y + height - 1 - i, state);
    drawLine(x + i, y + height - 1 - i, x + i, y + i, state);
  }
}

void GfxRenderer::drawArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir,
                          const int lineWidth, const bool state) const {
  const int stroke = std::min(lineWidth, maxRadius);
  const int innerRadius = std::max(maxRadius - stroke, 0);
  const int outerRadius = maxRadius;

  if (outerRadius <= 0) {
    return;
  }

  const int outerRadiusSq = outerRadius * outerRadius;
  const int innerRadiusSq = innerRadius * innerRadius;

  int xOuter = outerRadius;
  int xInner = innerRadius;

  for (int dy = 0; dy <= outerRadius; ++dy) {
    while (xOuter > 0 && (xOuter * xOuter + dy * dy) > outerRadiusSq) {
      --xOuter;
    }
    // Keep the smallest x that still lies outside/at the inner radius,
    // i.e. (x^2 + y^2) >= innerRadiusSq.
    while (xInner > 0 && ((xInner - 1) * (xInner - 1) + dy * dy) >= innerRadiusSq) {
      --xInner;
    }

    if (xOuter < xInner) {
      continue;
    }

    const int x0 = cx + xDir * xInner;
    const int x1 = cx + xDir * xOuter;
    const int left = std::min(x0, x1);
    const int width = std::abs(x1 - x0) + 1;
    const int py = cy + yDir * dy;

    if (width > 0) {
      fillRect(left, py, width, 1, state);
    }
  }
};

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool state) const {
  drawRoundedRect(x, y, width, height, lineWidth, cornerRadius, true, true, true, true, state);
}

// Border is inside the rectangle, rounded corners
void GfxRenderer::drawRoundedRect(const int x, const int y, const int width, const int height, const int lineWidth,
                                  const int cornerRadius, bool roundTopLeft, bool roundTopRight, bool roundBottomLeft,
                                  bool roundBottomRight, bool state) const {
  if (lineWidth <= 0 || width <= 0 || height <= 0) {
    return;
  }

  const int maxRadius = std::min({cornerRadius, width / 2, height / 2});
  if (maxRadius <= 0) {
    drawRect(x, y, width, height, lineWidth, state);
    return;
  }

  const int stroke = std::min(lineWidth, maxRadius);
  const int right = x + width - 1;
  const int bottom = y + height - 1;

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    if (roundTopLeft || roundTopRight) {
      fillRect(x + maxRadius, y, horizontalWidth, stroke, state);
    }
    if (roundBottomLeft || roundBottomRight) {
      fillRect(x + maxRadius, bottom - stroke + 1, horizontalWidth, stroke, state);
    }
  }

  const int verticalHeight = height - 2 * maxRadius;
  if (verticalHeight > 0) {
    if (roundTopLeft || roundBottomLeft) {
      fillRect(x, y + maxRadius, stroke, verticalHeight, state);
    }
    if (roundTopRight || roundBottomRight) {
      fillRect(right - stroke + 1, y + maxRadius, stroke, verticalHeight, state);
    }
  }

  if (roundTopLeft) {
    drawArc(maxRadius, x + maxRadius, y + maxRadius, -1, -1, lineWidth, state);
  }
  if (roundTopRight) {
    drawArc(maxRadius, right - maxRadius, y + maxRadius, 1, -1, lineWidth, state);
  }
  if (roundBottomRight) {
    drawArc(maxRadius, right - maxRadius, bottom - maxRadius, 1, 1, lineWidth, state);
  }
  if (roundBottomLeft) {
    drawArc(maxRadius, x + maxRadius, bottom - maxRadius, -1, 1, lineWidth, state);
  }
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  if (state) {
    fillRectImpl<Color::Black>(x, y, width, height);
  } else {
    fillRectImpl<Color::White>(x, y, width, height);
  }
}

// NOTE: Those are in critical path, and need to be templated to avoid runtime checks for every pixel.
// Any branching must be done outside the loops to avoid performance degradation.
template <>
void GfxRenderer::drawPixelDither<Color::Clear>(const int x, const int y) const {
  // Do nothing
}

template <>
void GfxRenderer::drawPixelDither<Color::Black>(const int x, const int y) const {
  drawPixel(x, y, true);
}

template <>
void GfxRenderer::drawPixelDither<Color::White>(const int x, const int y) const {
  drawPixel(x, y, false);
}

template <>
void GfxRenderer::drawPixelDither<Color::LightGray>(const int x, const int y) const {
  drawPixel(x, y, x % 2 == 0 && y % 2 == 0);
}

template <>
void GfxRenderer::drawPixelDither<Color::DarkGray>(const int x, const int y) const {
  drawPixel(x, y, (x + y) % 2 == 0);  // TODO: maybe find a better pattern?
}

void GfxRenderer::fillRectDither(const int x, const int y, const int width, const int height, Color color) const {
  switch (color) {
    case Color::Clear:
      break;
    case Color::Black:
      fillRectImpl<Color::Black>(x, y, width, height);
      break;
    case Color::White:
      fillRectImpl<Color::White>(x, y, width, height);
      break;
    case Color::LightGray:
      fillRectImpl<Color::LightGray>(x, y, width, height);
      break;
    case Color::DarkGray:
      fillRectImpl<Color::DarkGray>(x, y, width, height);
      break;
  }
}

template <Color C>
void GfxRenderer::fillRectImpl(const int x, const int y, const int width, const int height) const {
  if constexpr (C == Color::Clear) return;
  if (width <= 0 || height <= 0) return;
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;

  // Clip in logical space.
  const int screenW = getScreenWidth();
  const int screenH = getScreenHeight();
  const int lx0 = std::max(0, x);
  const int ly0 = std::max(0, y);
  const int lx1 = std::min(screenW, x + width);
  const int ly1 = std::min(screenH, y + height);
  if (lx0 >= lx1 || ly0 >= ly1) return;

  // Rotate the two opposing logical corners into physical-framebuffer space.
  // The bounding rect in physical space is the rect we need to fill — rotation
  // is rigid (no shear/stretch) so the bbox of the two corners IS the rect.
  int paX, paY, pbX, pbY;
  rotateCoordinates(orientation, lx0, ly0, &paX, &paY, panelWidth, panelHeight);
  rotateCoordinates(orientation, lx1 - 1, ly1 - 1, &pbX, &pbY, panelWidth, panelHeight);

  const int phyX0 = std::min(paX, pbX);
  const int phyX1 = std::max(paX, pbX);  // inclusive
  int phyY0 = std::min(paY, pbY);
  int phyY1 = std::max(paY, pbY);

  // Strip mode: clip Y range to the active band and redirect writes.
  uint8_t* target = getWriteTarget();
  const int originY = getWriteOriginY();
  const int writeRows = getWriteRows();
  phyY0 = std::max(phyY0, originY);
  phyY1 = std::min(phyY1, originY + writeRows - 1);
  if (phyY0 > phyY1) return;

  // Bit/byte layout: MSB-first within a byte, so phyX → bit (7 - (phyX & 7)).
  // Head and tail masks cover only the in-rect bits of the first/last byte.
  const int byteStart = phyX0 >> 3;
  const int byteEnd = phyX1 >> 3;  // inclusive
  const uint8_t headMask = static_cast<uint8_t>(0xFFu >> (phyX0 & 7));
  const uint8_t tailMask = static_cast<uint8_t>(0xFFu << (7 - (phyX1 & 7)));
  const int32_t panelStride = static_cast<int32_t>(panelWidthBytes);

  if constexpr (C == Color::Black || C == Color::White) {
    // Solid fill. Framebuffer: 0 = black, 1 = white.
    const uint8_t fillByte = (C == Color::Black) ? 0x00u : 0xFFu;
    for (int py = phyY0; py <= phyY1; ++py) {
      uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t mask = headMask & tailMask;
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~mask);
        } else {
          row[byteStart] |= mask;
        }
      } else {
        if constexpr (C == Color::Black) {
          row[byteStart] &= static_cast<uint8_t>(~headMask);
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] &= static_cast<uint8_t>(~tailMask);
        } else {
          row[byteStart] |= headMask;
          if (byteEnd > byteStart + 1) {
            memset(row + byteStart + 1, fillByte, byteEnd - byteStart - 1);
          }
          row[byteEnd] |= tailMask;
        }
      }
    }
  } else {
    // Dither (LightGray / DarkGray). Both patterns have period 2 in logical
    // (x, y), so per physical row we precompute one byte that represents the
    // pattern across an 8-pixel stretch — every full byte in the row uses
    // that same value.
    //
    // dlxPerPhyX / dlyPerPhyX: how logical (x, y) change as phyX increments
    // along a physical row. Derived from inverting rotateCoordinates.
    int dlxPerPhyX = 0, dlyPerPhyX = 0;
    switch (orientation) {
      case Portrait:
        dlxPerPhyX = 0;
        dlyPerPhyX = 1;
        break;
      case PortraitInverted:
        dlxPerPhyX = 0;
        dlyPerPhyX = -1;
        break;
      case LandscapeClockwise:
        dlxPerPhyX = -1;
        dlyPerPhyX = 0;
        break;
      case LandscapeCounterClockwise:
        dlxPerPhyX = 1;
        dlyPerPhyX = 0;
        break;
    }

    // The dither pattern has period 2 in logical space, and each orientation
    // maps py to logical coords with a fixed parity relationship. The
    // blackMask byte therefore repeats with period 2 in py. Precompute both
    // variants outside the row loop to eliminate the per-row switch + 8-bit
    // construction loop.
    uint8_t blackMasks[2];
    for (int parityIdx = 0; parityIdx < 2; ++parityIdx) {
      const int samplePy = phyY0 + parityIdx;
      int lxBase = 0, lyBase = 0;
      switch (orientation) {
        case Portrait:
          lxBase = panelHeight - 1 - samplePy;
          lyBase = byteStart * 8;
          break;
        case PortraitInverted:
          lxBase = samplePy;
          lyBase = panelWidth - 1 - byteStart * 8;
          break;
        case LandscapeClockwise:
          lxBase = panelWidth - 1 - byteStart * 8;
          lyBase = panelHeight - 1 - samplePy;
          break;
        case LandscapeCounterClockwise:
          lxBase = byteStart * 8;
          lyBase = samplePy;
          break;
      }
      uint8_t mask = 0;
      for (int b = 0; b < 8; ++b) {
        const int lx = lxBase + b * dlxPerPhyX;
        const int ly = lyBase + b * dlyPerPhyX;
        bool isBlack;
        if constexpr (C == Color::LightGray) {
          isBlack = ((lx & 1) == 0) && ((ly & 1) == 0);
        } else {  // DarkGray
          isBlack = (((lx + ly) & 1) == 0);
        }
        if (isBlack) mask |= static_cast<uint8_t>(1u << (7 - b));
      }
      blackMasks[samplePy & 1] = mask;
    }

    for (int py = phyY0; py <= phyY1; ++py) {
      const uint8_t blackMask = blackMasks[py & 1];
      const uint8_t whiteMask = static_cast<uint8_t>(~blackMask);

      // Dither writes BOTH inks (the slow path called drawPixel for every
      // pixel — setting or clearing — so we must do the same). Inside the
      // rect mask: write whiteMask (1s where white, 0s where black). Outside
      // the rect mask: leave the framebuffer untouched.
      uint8_t* row = target + static_cast<int32_t>(py - originY) * panelStride;
      if (byteStart == byteEnd) {
        const uint8_t rectMask = headMask & tailMask;
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~rectMask) | (rectMask & whiteMask));
      } else {
        row[byteStart] = static_cast<uint8_t>((row[byteStart] & ~headMask) | (headMask & whiteMask));
        if (byteEnd > byteStart + 1) {
          // Period 2, so every full byte in this row is exactly whiteMask.
          memset(row + byteStart + 1, whiteMask, byteEnd - byteStart - 1);
        }
        row[byteEnd] = static_cast<uint8_t>((row[byteEnd] & ~tailMask) | (tailMask & whiteMask));
      }
    }
  }
}

template void GfxRenderer::fillRectImpl<Color::Black>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::White>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::LightGray>(int, int, int, int) const;
template void GfxRenderer::fillRectImpl<Color::DarkGray>(int, int, int, int) const;

void GfxRenderer::maskRoundedRectOutsideCorners(const int x, const int y, const int width, const int height,
                                                const int radius, const Color color) const {
  if (radius <= 0 || color == Color::Clear) {
    return;
  }

  const int rr = radius - 1;
  const int rr2 = rr * rr;
  for (int dy = 0; dy < radius; dy++) {
    for (int dx = 0; dx < radius; dx++) {
      const int tx = rr - dx;
      const int ty = rr - dy;
      if (tx * tx + ty * ty > rr2) {
        if (color == Color::White || color == Color::Black) {
          bool state = color == Color::Black;
          drawPixel(x + dx, y + dy, state);                           // top-left
          drawPixel(x + width - 1 - dx, y + dy, state);               // top-right
          drawPixel(x + dx, y + height - 1 - dy, state);              // bottom-left
          drawPixel(x + width - 1 - dx, y + height - 1 - dy, state);  // bottom-right
        } else if (color == Color::LightGray) {
          drawPixelDither<Color::LightGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::LightGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::LightGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        } else if (color == Color::DarkGray) {
          drawPixelDither<Color::DarkGray>(x + dx, y + dy);                           // top-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + dy);               // top-right
          drawPixelDither<Color::DarkGray>(x + dx, y + height - 1 - dy);              // bottom-left
          drawPixelDither<Color::DarkGray>(x + width - 1 - dx, y + height - 1 - dy);  // bottom-right
        }
      }
    }
  }
}

template <Color color>
void GfxRenderer::fillArc(const int maxRadius, const int cx, const int cy, const int xDir, const int yDir) const {
  if (maxRadius <= 0) return;

  if constexpr (color == Color::Clear) {
    return;
  }

  const int radiusSq = maxRadius * maxRadius;

  // Avoid sqrt by scanning from outer radius inward while y grows.
  int x = maxRadius;
  for (int dy = 0; dy <= maxRadius; ++dy) {
    while (x > 0 && (x * x + dy * dy) > radiusSq) {
      --x;
    }
    if (x < 0) break;

    const int py = cy + yDir * dy;
    if (py < 0 || py >= getScreenHeight()) continue;

    int x0 = cx;
    int x1 = cx + xDir * x;
    if (x0 > x1) std::swap(x0, x1);
    const int width = x1 - x0 + 1;

    if (width <= 0) continue;

    if constexpr (color == Color::Black) {
      fillRect(x0, py, width, 1, true);
    } else if constexpr (color == Color::White) {
      fillRect(x0, py, width, 1, false);
    } else {
      // LightGray / DarkGray: use existing dithered fill path.
      fillRectDither(x0, py, width, 1, color);
    }
  }
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  const Color color) const {
  fillRoundedRect(x, y, width, height, cornerRadius, true, true, true, true, color);
}

void GfxRenderer::fillRoundedRect(const int x, const int y, const int width, const int height, const int cornerRadius,
                                  bool roundTopLeft, bool roundTopRight, bool roundBottomLeft, bool roundBottomRight,
                                  const Color color) const {
  if (width <= 0 || height <= 0) {
    return;
  }

  // Assume if we're not rounding all corners then we are only rounding one side
  const int roundedSides = (!roundTopLeft || !roundTopRight || !roundBottomLeft || !roundBottomRight) ? 1 : 2;
  const int maxRadius = std::min({cornerRadius, width / roundedSides, height / roundedSides});
  if (maxRadius <= 0) {
    fillRectDither(x, y, width, height, color);
    return;
  }

  const int horizontalWidth = width - 2 * maxRadius;
  if (horizontalWidth > 0) {
    fillRectDither(x + maxRadius + 1, y, horizontalWidth - 2, height, color);
  }

  const int leftFillTop = y + (roundTopLeft ? (maxRadius + 1) : 0);
  const int leftFillBottom = y + height - 1 - (roundBottomLeft ? (maxRadius + 1) : 0);
  if (leftFillBottom >= leftFillTop) {
    fillRectDither(x, leftFillTop, maxRadius + 1, leftFillBottom - leftFillTop + 1, color);
  }

  const int rightFillTop = y + (roundTopRight ? (maxRadius + 1) : 0);
  const int rightFillBottom = y + height - 1 - (roundBottomRight ? (maxRadius + 1) : 0);
  if (rightFillBottom >= rightFillTop) {
    fillRectDither(x + width - maxRadius - 1, rightFillTop, maxRadius + 1, rightFillBottom - rightFillTop + 1, color);
  }

  auto fillArcTemplated = [this](int maxRadius, int cx, int cy, int xDir, int yDir, Color color) {
    switch (color) {
      case Color::Clear:
        break;
      case Color::Black:
        fillArc<Color::Black>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::White:
        fillArc<Color::White>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::LightGray:
        fillArc<Color::LightGray>(maxRadius, cx, cy, xDir, yDir);
        break;
      case Color::DarkGray:
        fillArc<Color::DarkGray>(maxRadius, cx, cy, xDir, yDir);
        break;
    }
  };

  if (roundTopLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + maxRadius, -1, -1, color);
  }

  if (roundTopRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + maxRadius, 1, -1, color);
  }

  if (roundBottomRight) {
    fillArcTemplated(maxRadius, x + width - maxRadius - 1, y + height - maxRadius - 1, 1, 1, color);
  }

  if (roundBottomLeft) {
    fillArcTemplated(maxRadius, x + maxRadius, y + height - maxRadius - 1, -1, 1, color);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(orientation, x, y, &rotatedX, &rotatedY, panelWidth, panelHeight);
  // Rotate origin corner
  switch (orientation) {
    case Portrait:
      rotatedY = rotatedY - height;
      break;
    case PortraitInverted:
      rotatedX = rotatedX - width;
      break;
    case LandscapeClockwise:
      rotatedY = rotatedY - height;
      rotatedX = rotatedX - width;
      break;
    case LandscapeCounterClockwise:
      break;
  }
  // TODO: Rotate bits
  display.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawIcon(const uint8_t bitmap[], const int x, const int y, const int size) const {
  // Plot the icon pixel-by-pixel through drawPixel (which applies the orientation
  // transform) instead of the byte-aligned framebuffer blit. The blit snaps the
  // icon's position to 8px (one byte) along the rotated axis, which prevents it
  // from aligning with adjacent text; per-pixel plotting is pixel-precise.
  // Icons are square and 1bpp (MSB-first, bit==0 = ink). The (size-1-row, col)
  // mapping reproduces the Portrait orientation the blit produced; drawIcon is
  // only called by the UI themes, which all render in forced Portrait.
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; row++) {
    for (int col = 0; col < size; col++) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) {
        drawPixel(x + (size - 1 - row), y + col, true);
      }
    }
  }
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
  if (fontCacheManager_ && fontCacheManager_->isScanning()) return;
  // For 1-bit bitmaps, use optimized 1-bit rendering path (no crop support for 1-bit)
  if (bitmap.is1Bit() && cropX == 0.0f && cropY == 0.0f) {
    drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight);
    return;
  }

  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);
  LOG_DBG("GFX", "Cropping %dx%d by %dx%d pix, is %s", bitmap.getWidth(), bitmap.getHeight(), cropPixX, cropPixY,
          bitmap.isTopDown() ? "top-down" : "bottom-up");

  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  bool hasTargetBounds = false;
  float fitScale = 1.0f;

  if (maxWidth > 0 && croppedWidth > 0.0f) {
    fitScale = static_cast<float>(maxWidth) / croppedWidth;
    hasTargetBounds = true;
  }

  if (maxHeight > 0 && croppedHeight > 0.0f) {
    const float heightScale = static_cast<float>(maxHeight) / croppedHeight;
    fitScale = hasTargetBounds ? std::min(fitScale, heightScale) : heightScale;
    hasTargetBounds = true;
  }

  if (hasTargetBounds && fitScale < 1.0f) {
    scale = fitScale;
    isScaled = true;
  }
  LOG_DBG("GFX", "Scaling by %f - %s", scale, isScaled ? "scaled" : "not scaled");

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  if (!outputRow || !rowBytes) {
    LOG_ERR("GFX", "!! Failed to allocate BMP row buffers");
    free(outputRow);
    free(rowBytes);
    return;
  }

  for (int bmpY = 0; bmpY < (bitmap.getHeight() - cropPixY); bmpY++) {
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
      break;
    }

    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      return;
    }

    if (screenY < 0) {
      continue;
    }

    if (bmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

    for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      const uint8_t rawVal = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;
      // Dark mode: draw the bitmap PRE-INVERTED so the global panel-push
      // inversion (see setDarkMode) cancels out and photographic content --
      // covers, sleep images -- keeps its true polarity while the chrome
      // around it inverts. Kindle-style: dark UI, normal covers.
      const uint8_t val = darkMode_ ? static_cast<uint8_t>(3 - rawVal) : rawVal;

      if (renderMode == BW && val < 3) {
        drawPixel(screenX, screenY);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
        drawPixel(screenX, screenY, false);
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        drawPixel(screenX, screenY, false);
      }
    }
  }

  free(outputRow);
  free(rowBytes);

  const int sourceWidth = bitmap.getWidth() - cropPixX * 2;
  const int sourceHeight = bitmap.getHeight() - cropPixY * 2;
  const int renderedWidth = isScaled ? static_cast<int>(std::floor((sourceWidth - 1) * scale)) + 1 : sourceWidth;
  const int renderedHeight = isScaled ? static_cast<int>(std::floor((sourceHeight - 1) * scale)) + 1 : sourceHeight;
  preserveImagePolarity(x, y, renderedWidth, renderedHeight);
}

void GfxRenderer::drawBitmap1Bit(const Bitmap& bitmap, const int x, const int y, const int maxWidth,
                                 const int maxHeight) const {
  float scale = 1.0f;
  bool isScaled = false;
  if (maxWidth > 0 && bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>(bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>(bitmap.getHeight()));
    isScaled = true;
  }

  // For 1-bit BMP, output is still 2-bit packed (for consistency with readNextRow)
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* rowBytes = static_cast<uint8_t*>(malloc(bitmap.getRowBytes()));

  // Downscale accumulators: per-destination-column black/total source-pixel
  // counts for the destination row currently being gathered. Sources are
  // DITHERED 1-bit covers -- with the previous any-black-wins sampling, every
  // mid-gray dot pattern collapsed to solid black once several source pixels
  // landed on one screen pixel, turning the My Books row and grid cells into
  // high-contrast black blobs. Box-filtering each bucket and re-dithering the
  // average preserves the tone instead.
  uint16_t* blackCount = nullptr;
  uint16_t* totalCount = nullptr;
  int destW = 0;
  if (isScaled) {
    destW = static_cast<int>(std::floor((bitmap.getWidth() - 1) * scale)) + 1;
    blackCount = static_cast<uint16_t*>(calloc(destW, sizeof(uint16_t)));
    totalCount = static_cast<uint16_t*>(calloc(destW, sizeof(uint16_t)));
  }

  if (!outputRow || !rowBytes || (isScaled && (!blackCount || !totalCount))) {
    LOG_ERR("GFX", "!! Failed to allocate 1-bit BMP row buffers");
    free(outputRow);
    free(rowBytes);
    free(blackCount);
    free(totalCount);
    return;
  }

  // Emit one accumulated destination row: 3-tone re-dither of the bucket
  // averages -- dark buckets go black, light ones stay white, and mid-tones
  // render as a pixel checkerboard so downscaled dither still reads as gray.
  auto flushDestRow = [&](const int destY) {
    const int screenY = y + destY;
    if (screenY >= 0 && screenY < getScreenHeight()) {
      for (int destX = 0; destX < destW; destX++) {
        const int screenX = x + destX;
        if (screenX < 0 || screenX >= getScreenWidth() || totalCount[destX] == 0) continue;
        const uint32_t black3 = 3u * blackCount[destX];
        const uint32_t total = totalCount[destX];
        // black3/total in [0,3]: >=2 black, <1 white, in between checkerboard.
        const bool drawBlack = black3 >= 2 * total || (black3 >= total && ((screenX + screenY) & 1) == 0);
        // Dark mode: pre-invert (see drawBitmap) so the panel-push inversion
        // restores the cover's true polarity.
        if (darkMode_ ? !drawBlack : drawBlack) {
          drawPixel(screenX, screenY, true);
        }
      }
    }
    memset(blackCount, 0, destW * sizeof(uint16_t));
    memset(totalCount, 0, destW * sizeof(uint16_t));
  };
  int pendingDestY = -1;  // destination row currently accumulating (-1 = none)

  for (int bmpY = 0; bmpY < bitmap.getHeight(); bmpY++) {
    // Read rows sequentially using readNextRow
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      LOG_ERR("GFX", "Failed to read row %d from 1-bit bitmap", bmpY);
      free(outputRow);
      free(rowBytes);
      free(blackCount);
      free(totalCount);
      return;
    }

    // Calculate screen Y based on whether BMP is top-down or bottom-up
    const int bmpYOffset = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;

    if (isScaled) {
      // bmpYOffset moves monotonically (up or down), so every source row of a
      // given destination row arrives consecutively: accumulate until it changes.
      const int destY = static_cast<int>(std::floor(bmpYOffset * scale));
      if (pendingDestY >= 0 && destY != pendingDestY) {
        flushDestRow(pendingDestY);
      }
      pendingDestY = destY;
      for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
        const int destX = static_cast<int>(std::floor(bmpX * scale));
        if (destX < 0 || destX >= destW) continue;
        totalCount[destX]++;
        const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;
        if (val < 3) blackCount[destX]++;
      }
      continue;
    }

    const int screenY = y + bmpYOffset;
    if (screenY >= getScreenHeight()) {
      continue;  // Continue reading to keep row counter in sync
    }
    if (screenY < 0) {
      continue;
    }

    for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
      int screenX = x + bmpX;
      if (screenX >= getScreenWidth()) {
        break;
      }
      if (screenX < 0) {
        continue;
      }

      // Get 2-bit value (result of readNextRow quantization)
      const uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;

      // For 1-bit source: 0 or 1 -> map to black (0,1,2) or white (3)
      // val < 3 means black pixel (draw it). Dark mode: pre-invert (see
      // drawBitmap) so the panel-push inversion restores true polarity --
      // the WHITE source pixels get drawn instead.
      if (darkMode_ ? (val == 3) : (val < 3)) {
        drawPixel(screenX, screenY, true);
      }
    }
  }

  if (isScaled && pendingDestY >= 0) {
    flushDestRow(pendingDestY);
  }

  free(outputRow);
  free(rowBytes);
  free(blackCount);
  free(totalCount);

  const int renderedWidth =
      isScaled ? static_cast<int>(std::floor((bitmap.getWidth() - 1) * scale)) + 1 : bitmap.getWidth();
  const int renderedHeight =
      isScaled ? static_cast<int>(std::floor((bitmap.getHeight() - 1) * scale)) + 1 : bitmap.getHeight();
  preserveImagePolarity(x, y, renderedWidth, renderedHeight);
}

void GfxRenderer::preserveImagePolarity(const int x, const int y, const int width, const int height) const {
  if (renderMode != BW || !display.isInverted() || _stripActive || !frameBuffer || width <= 0 || height <= 0) {
    return;
  }

  int ax, ay, bx, by;
  rotateCoordinates(orientation, x, y, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x + width - 1, y + height - 1, &bx, &by, panelWidth, panelHeight);

  int left = std::max(0, std::min(ax, bx));
  int right = std::min(static_cast<int>(panelWidth) - 1, std::max(ax, bx));
  int top = std::max(0, std::min(ay, by));
  int bottom = std::min(static_cast<int>(panelHeight) - 1, std::max(ay, by));
  if (left > right || top > bottom) return;

  for (int row = top; row <= bottom; row++) {
    uint8_t* rowData = frameBuffer + static_cast<uint32_t>(row) * panelWidthBytes;
    int col = left;
    while (col <= right && (col & 7) != 0) {
      rowData[col >> 3] ^= static_cast<uint8_t>(0x80U >> (col & 7));
      col++;
    }
    while (col + 7 <= right) {
      rowData[col >> 3] ^= 0xFF;
      col += 8;
    }
    while (col <= right) {
      rowData[col >> 3] ^= static_cast<uint8_t>(0x80U >> (col & 7));
      col++;
    }
  }
}

void GfxRenderer::fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state) const {
  if (numPoints < 3) return;

  // Find bounding box
  int minY = yPoints[0], maxY = yPoints[0];
  for (int i = 1; i < numPoints; i++) {
    if (yPoints[i] < minY) minY = yPoints[i];
    if (yPoints[i] > maxY) maxY = yPoints[i];
  }

  // Clip to screen
  if (minY < 0) minY = 0;
  if (maxY >= getScreenHeight()) maxY = getScreenHeight() - 1;

  // Allocate node buffer for scanline algorithm
  auto* nodeX = static_cast<int*>(malloc(numPoints * sizeof(int)));
  if (!nodeX) {
    LOG_ERR("GFX", "!! Failed to allocate polygon node buffer");
    return;
  }

  // Scanline fill algorithm
  for (int scanY = minY; scanY <= maxY; scanY++) {
    int nodes = 0;

    // Find all intersection points with edges
    int j = numPoints - 1;
    for (int i = 0; i < numPoints; i++) {
      if ((yPoints[i] < scanY && yPoints[j] >= scanY) || (yPoints[j] < scanY && yPoints[i] >= scanY)) {
        // Calculate X intersection using fixed-point to avoid float
        int dy = yPoints[j] - yPoints[i];
        if (dy != 0) {
          nodeX[nodes++] = xPoints[i] + (scanY - yPoints[i]) * (xPoints[j] - xPoints[i]) / dy;
        }
      }
      j = i;
    }

    // Sort nodes by X
    std::sort(nodeX, nodeX + nodes);

    // Fill between pairs of nodes
    for (int i = 0; i < nodes - 1; i += 2) {
      int startX = nodeX[i];
      int endX = nodeX[i + 1];

      // Clip to screen
      if (startX < 0) startX = 0;
      if (endX >= getScreenWidth()) endX = getScreenWidth() - 1;

      // Draw horizontal line
      for (int x = startX; x <= endX; x++) {
        drawPixel(x, scanY, state);
      }
    }
  }

  free(nodeX);
}

// For performance measurement (using static to allow "const" methods)
static unsigned long start_ms = 0;

void GfxRenderer::clearScreen(const uint8_t color) const {
  start_ms = millis();
  if (_stripActive) {
    // Clear only the active band's scratch, not the shared framebuffer.
    memset(_stripBuf, color, static_cast<size_t>(panelWidthBytes) * _stripRows);
    return;
  }
  display.clearScreen(color);
}

void GfxRenderer::beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const {
  // Band is caller-guaranteed in-bounds (the reader's grayscale loop computes
  // it); assert catches future misuse in debug before it mis-renders or wraps
  // the downstream uint16_t cast in writeGrayscalePlaneStrip.
  assert(scratch != nullptr && stripRows > 0 && stripY0 >= 0 && stripY0 <= static_cast<int>(panelHeight) - stripRows);
  _stripBuf = scratch;
  _stripY0 = stripY0;
  _stripRows = stripRows;
  _stripActive = true;
}

void GfxRenderer::endStripTarget() const {
  _stripActive = false;
  _stripBuf = nullptr;
  _stripY0 = 0;
  _stripRows = 0;
}

bool GfxRenderer::glyphIntersectsStrip(int x0, int y0, int x1, int y1) const {
  if (!_stripActive) {
    return true;
  }
  // Rotate the two opposite bbox corners to physical coords. For 90-degree
  // orientations the physical bbox stays axis-aligned, so min/max of the two
  // rotated corners' Y bounds the glyph's physical y-extent.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x0, y0, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x1, y1, &bx, &by, panelWidth, panelHeight);
  const int minY = ay < by ? ay : by;
  const int maxY = ay > by ? ay : by;
  return !(maxY < _stripY0 || minY >= _stripY0 + _stripRows);
}

void GfxRenderer::invertScreen() const {
  for (uint32_t i = 0; i < frameBufferSize; i++) {
    frameBuffer[i] = ~frameBuffer[i];
  }
}

void GfxRenderer::displayBuffer(const HalDisplay::RefreshMode refreshMode, const bool forceCleanBaseOnHalf) const {
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBuffer", elapsed);
  // Dark mode inverts only for the panel push, then restores -- everything
  // that reads the framebuffer afterwards sees normal polarity (see
  // setDarkMode). Two 48KB XOR passes ~= 1-2ms at 160MHz, negligible next to
  // the e-ink refresh itself. Same wrap on every other pixel-push below.
  if (darkMode_) invertScreen();
  display.displayBuffer(refreshMode, fadingFix, forceCleanBaseOnHalf);
  if (darkMode_) invertScreen();
}

void GfxRenderer::displayBufferAsync(const HalDisplay::RefreshMode refreshMode, const bool forceCleanBaseOnHalf) const {
  // Fading fix relies on turnOffScreen, which the async SDK primitive has no
  // hook for at all -- keep those users on the blocking path unconditionally.
  if (fadingFix) {
    displayBuffer(refreshMode, forceCleanBaseOnHalf);
    return;
  }
  auto elapsed = millis() - start_ms;
  LOG_DBG("GFX", "Time = %lu ms from clearScreen to displayBufferAsync", elapsed);
  // Safe to un-invert once this call returns even though the panel is still
  // refreshing: the SDK's "push" step (SPI transfer into controller RAM) is
  // synchronous and already complete by the time displayBufferAsync returns;
  // the panel refreshes afterward from ITS OWN copy, not by re-reading ours.
  if (darkMode_) invertScreen();
  display.displayBufferAsync(refreshMode, forceCleanBaseOnHalf);
  if (darkMode_) invertScreen();
}

void GfxRenderer::waitRefreshComplete() const { display.waitRefreshComplete(); }

bool GfxRenderer::supportsAsyncRefresh() const { return !fadingFix && display.supportsAsyncRefresh(); }

size_t GfxRenderer::readFramebufferRegion(int x, int y, int w, int h, uint8_t* dst, size_t dstCapacity) const {
  if (dst == nullptr || w <= 0 || h <= 0 || !frameBuffer) return 0;

  const AlignedMemRect mem = screenRectToAlignedMemRect(orientation, x, y, w, h, panelWidth, panelHeight);
  if (!mem.valid) return 0;

  const size_t rowBytes = mem.w / 8;  // exact: mem.w is a multiple of 8
  const size_t needed = rowBytes * mem.h;
  if (needed > dstCapacity) return 0;

  for (uint16_t row = 0; row < mem.h; ++row) {
    const uint8_t* srcRow = frameBuffer + (static_cast<uint32_t>(mem.y + row) * panelWidthBytes) + (mem.x / 8);
    uint8_t* dstRow = dst + (static_cast<size_t>(row) * rowBytes);
    memcpy(dstRow, srcRow, rowBytes);
  }
  return needed;
}

void GfxRenderer::writeFramebufferRegion(int x, int y, int w, int h, const uint8_t* src) {
  if (src == nullptr || w <= 0 || h <= 0 || !frameBuffer) return;

  const AlignedMemRect mem = screenRectToAlignedMemRect(orientation, x, y, w, h, panelWidth, panelHeight);
  if (!mem.valid) return;

  const size_t rowBytes = mem.w / 8;  // exact: mem.w is a multiple of 8

  for (uint16_t row = 0; row < mem.h; ++row) {
    const uint8_t* srcRow = src + (static_cast<size_t>(row) * rowBytes);
    uint8_t* dstRow = frameBuffer + (static_cast<uint32_t>(mem.y + row) * panelWidthBytes) + (mem.x / 8);
    memcpy(dstRow, srcRow, rowBytes);
  }
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  if (!text || maxWidth <= 0) return "";

  std::string item = text;
  // U+2026 HORIZONTAL ELLIPSIS (UTF-8: 0xE2 0x80 0xA6)
  const char* ellipsis = "\xe2\x80\xa6";
  int textWidth = getTextWidth(fontId, item.c_str(), style);
  if (textWidth <= maxWidth) {
    // Text fits, return as is
    return item;
  }

  while (!item.empty() && getTextWidth(fontId, (item + ellipsis).c_str(), style) >= maxWidth) {
    utf8RemoveLastChar(item);
  }

  return item.empty() ? ellipsis : item + ellipsis;
}

std::vector<std::string> GfxRenderer::wrappedText(const int fontId, const char* text, const int maxWidth,
                                                  const int maxLines, const EpdFontFamily::Style style) const {
  std::vector<std::string> lines;

  if (!text || maxWidth <= 0 || maxLines <= 0) return lines;

  std::string remaining = text;
  std::string currentLine;

  while (!remaining.empty()) {
    if (static_cast<int>(lines.size()) == maxLines - 1) {
      // Last available line: combine any word already started on this line with
      // the rest of the text, then let truncatedText fit it with an ellipsis.
      std::string lastContent = currentLine.empty() ? remaining : currentLine + " " + remaining;
      lines.push_back(truncatedText(fontId, lastContent.c_str(), maxWidth, style));
      return lines;
    }

    // Find next word
    size_t spacePos = remaining.find(' ');
    std::string word;

    if (spacePos == std::string::npos) {
      word = remaining;
      remaining.clear();
    } else {
      word = remaining.substr(0, spacePos);
      remaining.erase(0, spacePos + 1);
    }

    std::string testLine = currentLine.empty() ? word : currentLine + " " + word;

    if (getTextWidth(fontId, testLine.c_str(), style) <= maxWidth) {
      currentLine = testLine;
    } else {
      if (!currentLine.empty()) {
        lines.push_back(currentLine);
        // If the carried-over word itself exceeds maxWidth, truncate it and
        // push it as a complete line immediately — storing it in currentLine
        // would allow a subsequent short word to be appended after the ellipsis.
        if (getTextWidth(fontId, word.c_str(), style) > maxWidth) {
          lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
          currentLine.clear();
          if (static_cast<int>(lines.size()) >= maxLines) return lines;
        } else {
          currentLine = word;
        }
      } else {
        // Single word wider than maxWidth: truncate and stop to avoid complicated
        // splitting rules (different between languages). Results in an aesthetically
        // pleasing end.
        lines.push_back(truncatedText(fontId, word.c_str(), maxWidth, style));
        return lines;
      }
    }
  }

  if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(currentLine);
  }

  return lines;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return panelHeight;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return panelWidth;
  }
  return panelHeight;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return panelWidth;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return panelHeight;
  }
  return panelWidth;
}

void GfxRenderer::tapToLogical(float nx, float ny, int& outX, int& outY) const {
  int phyX = static_cast<int>(nx * panelWidth);
  int phyY = static_cast<int>(ny * panelHeight);
  if (phyX < 0) phyX = 0;
  if (phyX > panelWidth - 1) phyX = panelWidth - 1;
  if (phyY < 0) phyY = 0;
  if (phyY > panelHeight - 1) phyY = panelHeight - 1;

  switch (orientation) {
    case Portrait:
      outX = panelHeight - 1 - phyY;
      outY = phyX;
      break;
    case PortraitInverted:
      outX = phyY;
      outY = panelWidth - 1 - phyX;
      break;
    case LandscapeClockwise:
      outX = panelWidth - 1 - phyX;
      outY = panelHeight - 1 - phyY;
      break;
    case LandscapeCounterClockwise:
    default:
      outX = phyX;
      outY = phyY;
      break;
  }
}

// Translate a logical rect through rotateCoordinates and take the bounding
// box of its four corners on the physical panel. Output coords are inclusive
// and clamped. Returns false if the rect ends up fully off-panel.
static bool logicalRectToPhysicalBounds(GfxRenderer::Orientation orientation, int lx, int ly, int lw, int lh,
                                        uint16_t panelWidth, uint16_t panelHeight, int* outX0, int* outY0, int* outX1,
                                        int* outY1) {
  if (lw <= 0 || lh <= 0) return false;
  int minX = INT32_MAX;
  int minY = INT32_MAX;
  int maxX = INT32_MIN;
  int maxY = INT32_MIN;
  const int corners[4][2] = {{lx, ly}, {lx + lw - 1, ly}, {lx, ly + lh - 1}, {lx + lw - 1, ly + lh - 1}};
  for (auto& c : corners) {
    int phyX;
    int phyY;
    rotateCoordinates(orientation, c[0], c[1], &phyX, &phyY, panelWidth, panelHeight);
    if (phyX < minX) minX = phyX;
    if (phyY < minY) minY = phyY;
    if (phyX > maxX) maxX = phyX;
    if (phyY > maxY) maxY = phyY;
  }
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  if (maxX >= panelWidth) maxX = panelWidth - 1;
  if (maxY >= panelHeight) maxY = panelHeight - 1;
  if (minX > maxX || minY > maxY) return false;
  *outX0 = minX;
  *outY0 = minY;
  *outX1 = maxX;
  *outY1 = maxY;
  return true;
}

size_t GfxRenderer::getRegionByteSize(int lx, int ly, int lw, int lh) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return 0;
  }
  // x bounds are in pixels; widen to byte boundaries on either side so per-row
  // memcpy stays byte-aligned even when the logical rect doesn't.
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  return static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
}

bool GfxRenderer::copyRegionToBuffer(int lx, int ly, int lw, int lh, uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    const uint8_t* src = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(buf + row * bytesPerRow, src, bytesPerRow);
  }
  return true;
}

bool GfxRenderer::copyBufferToRegion(int lx, int ly, int lw, int lh, const uint8_t* buf, size_t bufSize) const {
  int x0, y0, x1, y1;
  if (!logicalRectToPhysicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return false;
  }
  const int byteX0 = x0 / 8;
  const int byteX1 = x1 / 8;
  const int bytesPerRow = byteX1 - byteX0 + 1;
  const int rowCount = y1 - y0 + 1;
  const size_t needed = static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
  if (bufSize < needed || !frameBuffer || !buf) return false;
  for (int row = 0; row < rowCount; row++) {
    uint8_t* dst = frameBuffer + (y0 + row) * panelWidthBytes + byteX0;
    memcpy(dst, buf + row * bytesPerRow, bytesPerRow);
  }
  return true;
}

int GfxRenderer::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  // Advance table fast-path for SD card fonts during layout
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const uint8_t resolvedStyle = resolveSdCardStyle(*sdIt->second, style);
    return fp4::toPixel(sdIt->second->getAdvance(' ', resolvedStyle));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  const EpdGlyph* spaceGlyph = fontIt->second.getGlyph(' ', style);
  return spaceGlyph ? fp4::toPixel(spaceGlyph->advanceX) : 0;  // snap 12.4 fixed-point to nearest pixel
}

// Layout-time query for kashida justification (see ParsedText::computeJustifyPlan):
// the fixed per-glyph width of the active Arabic font's own U+0640 TATWEEL glyph, or
// 0 if the font has none -- callers use 0 as "this font can't do kashida, fall back
// to inter-word gap stretching entirely."
int GfxRenderer::getKashidaGlyphWidth(const int fontId, const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(resolveArabicFontId(fontId));
  if (fontIt == fontMap.end()) return 0;
  const EpdGlyph* tatweel = fontIt->second.getGlyph(0x0640, style);
  return tatweel ? fp4::toPixel(tatweel->advanceX) : 0;
}

// Layout-time query: does this single word contain a legal kashida insertion point
// in the active Arabic font? See ArabicShaper::hasKashidaPoint for the exact rule.
bool GfxRenderer::wordHasKashidaPoint(const int fontId, const char* word, const EpdFontFamily::Style style) const {
  const int arabicFontId = resolveArabicFontId(fontId);
  const auto fontIt = fontMap.find(arabicFontId);
  if (fontIt == fontMap.end()) return false;
  const auto& font = fontIt->second;
  // SD fonts: answer existence from the RAM interval table -- getGlyph misses
  // here trigger a per-character on-demand SD load during layout (see
  // getArabicTextWidth for the full reasoning).
  const auto arSdIt = sdCardFonts_.find(arabicFontId);
  if (arSdIt != sdCardFonts_.end()) {
    SdCardFont* const sdFont = arSdIt->second;
    const uint8_t sdStyleIdx = resolveSdCardStyle(*sdFont, style);
    return ArabicShaper::hasKashidaPoint(word, [&](uint32_t c) { return sdFont->hasGlyph(c, sdStyleIdx); });
  }
  return ArabicShaper::hasKashidaPoint(word, [&](uint32_t c) { return font.getGlyph(c, style) != nullptr; });
}

int GfxRenderer::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  // Arabic words are drawn with the resolved Arabic font, whose size follows the
  // separate arabicFontSize setting -- independent of the Latin reading fontId.
  // The gap between two Arabic words must therefore be sized by that Arabic font's
  // own space glyph, not the Latin font's: when Arabic is set larger than the Latin
  // reading size, a Latin-sized gap is too small for the big Arabic glyphs and the
  // words appear to join (no kerning across the gap here, matching the paths below).
  if (ScriptDetector::isArabicCodepoint(leftCp) || ScriptDetector::isArabicCodepoint(rightCp)) {
    const int arabicFontId = resolveArabicFontId(fontId);
    if (arabicFontId != fontId) {
      const auto arSdIt = sdCardFonts_.find(arabicFontId);
      if (arSdIt != sdCardFonts_.end() && arSdIt->second->hasAdvanceTable()) {
        return fp4::toPixel(arSdIt->second->getAdvance(' ', resolveSdCardStyle(*arSdIt->second, style)));
      }
      const auto arIt = fontMap.find(arabicFontId);
      if (arIt != fontMap.end()) {
        const EpdGlyph* arSpace = arIt->second.getGlyph(' ', style);
        if (arSpace) return fp4::toPixel(arSpace->advanceX);
      }
    }
  }

  // Advance table fast-path for SD card fonts during layout.
  // Kern data is not loaded during layout (consistent with previous metadataOnly behavior),
  // so we return just the space advance without kerning.
  auto sdIt = sdCardFonts_.find(fontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    const uint8_t resolvedStyle = resolveSdCardStyle(*sdIt->second, style);
    return fp4::toPixel(sdIt->second->getAdvance(' ', resolvedStyle));
  }

  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const auto& font = fontIt->second;
  const EpdGlyph* spaceGlyph = font.getGlyph(' ', style);
  const int32_t spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;
  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(font.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(font.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceAdvanceFP + kernFP);
}

int GfxRenderer::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) return 0;
  const int kernFP = fontIt->second.getKerning(leftCp, rightCp, style);  // 4.4 fixed-point
  return fp4::toPixel(kernFP);                                           // snap 4.4 fixed-point to nearest pixel
}

int GfxRenderer::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  // EPUB layout (ParsedText::measureWordWidth) is the caller here, so this must agree with
  // drawText's dispatch: Arabic codepoints don't exist in fontId's (Latin) font, so without
  // this check every Arabic word would measure as 0px wide, collapsing the whole paragraph
  // onto one line-break group while drawArabicText still draws it at full width -- the words
  // would render on top of each other. getArabicTextWidth already sums shaped-glyph advances,
  // which is exactly this function's contract for an Arabic word.
  if (ScriptDetector::containsArabic(text)) {
    return getArabicTextWidth(fontId, text, style);
  }

  // Match the font drawText would use for CJK-bearing strings (see resolveTextFontId).
  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  // Measure the exact codepoint stream drawText renders: bidi-reordered per
  // UAX#9 (Arabic itself is dispatched to getArabicTextWidth/ArabicShaper
  // above and never reaches this path). Measuring the raw logical text for a
  // mixed RTL/LTR run can disagree with the reordered visual layout drawText
  // actually produces.
  std::string visual;
  text = resolveVisualText(text, visual, BidiUtils::BidiBaseDir::AUTO);

  // Advance table fast-path for SD card fonts during layout.
  // No kerning/ligature lookup — consistent with previous metadataOnly behavior
  // where kern/lig data was not loaded.
  auto sdIt = sdCardFonts_.find(resolvedFontId);
  if (sdIt != sdCardFonts_.end() && sdIt->second->hasAdvanceTable()) {
    int32_t widthFP = 0;
    const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;
    const uint8_t styleIdx = resolveSdCardStyle(*sdIt->second, style);
    const auto fontIt = fontMap.find(resolvedFontId);
    if (fontIt == fontMap.end()) {
      LOG_ERR("GFX", "Font %d not found", resolvedFontId);
      return 0;
    }
    const auto& font = fontIt->second;
    while (uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text))) {
      // RTL vowel marks (niqqud/harakat) are zero-advance overlays in drawText — no width.
      if (BidiUtils::isTransparentMark(cp)) {
        continue;
      }
      int32_t advFP = sdIt->second->getAdvance(cp, styleIdx);
      if (advFP == 0 && !utf8IsCombiningMark(cp)) {
        const EpdGlyph* glyph = font.getGlyph(cp, style);
        advFP = glyph ? glyph->advanceX : 0;
      }
      widthFP += isSupSub ? (advFP + 1) / 2 : advFP;
    }
    return fp4::toPixel(widthFP);
  }

  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return 0;
  }

  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  const auto& font = fontIt->second;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    // RTL vowel marks (niqqud/harakat) are zero-advance overlays in drawText — no width.
    if (BidiUtils::isTransparentMark(cp)) {
      continue;
    }
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) together,
    // matching drawText so measurement and rendering agree exactly.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);         // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    }
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }

  return fontIt->second.getData(EpdFontFamily::REGULAR)->advanceY;
}

int GfxRenderer::getLineHeight(const int fontId, const float compression) const {
  return static_cast<int>(getLineHeight(fontId) * compression + 0.5f);
}

int GfxRenderer::getTextHeight(const int fontId) const {
  const auto fontIt = fontMap.find(fontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", fontId);
    return 0;
  }
  return fontIt->second.getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  // Route CJK-bearing strings to the fallback font (see resolveTextFontId).
  const int resolvedFontId = resolveTextFontId(fontId, text, style);
  // Redirected to the SD fallback: batch-load the string's glyphs so the draw
  // loop below doesn't fault them in one SD read at a time (#2725).
  if (resolvedFontId != fontId) {
    ensureSdGlyphsResident(resolvedFontId, text, style, false);
  }
  const auto fontIt = fontMap.find(resolvedFontId);
  if (fontIt == fontMap.end()) {
    LOG_ERR("GFX", "Font %d not found", resolvedFontId);
    return;
  }

  const auto& font = fontIt->second;

  int lastBaseY = y;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap

  uint32_t cp;
  uint32_t prevCp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    // RTL vowel marks (Hebrew niqqud, Arabic harakat) ride the combining-mark
    // path: zero-advance overlays on the preceding base glyph (applyBidiVisual
    // emits base-then-marks per UAX#9 L3). anchorFor pins position-sensitive
    // niqqud (dagesh, shin/sin dots, holam) to their spot on the base; other
    // marks stay centered, raised above the base or (kasra) at their
    // font-native position. Fonts without their glyphs — the built-ins — miss
    // the getGlyph lookup and skip them, as before. (Arabic itself is
    // dispatched to drawArabicText/ArabicShaper above and never reaches this
    // loop; isTransparentMark's Arabic-harakat coverage is inert here.)
    if (utf8IsCombiningMark(cp) || BidiUtils::isTransparentMark(cp)) {
      const EpdGlyph* combiningGlyph = font.getGlyph(cp, style);
      if (!combiningGlyph) continue;
      const auto anchor = combiningMark::anchorFor(cp);
      const int raiseBy =
          combiningMark::raiseAboveBase(anchor, combiningGlyph->top, combiningGlyph->height, lastBaseTop);
      const int combiningX = x - raiseBy;
      const int combiningY = combiningMark::anchorOverRotated90CW(anchor, lastBaseY, lastBaseLeft, lastBaseWidth,
                                                                  combiningGlyph->left, combiningGlyph->width);
      renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, combiningX, combiningY, black, style);
      continue;
    }

    cp = font.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) as one unit,
    // subtracting for the rotated coordinate direction.
    if (prevCp != 0) {
      const auto kernFP = font.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      lastBaseY -= fp4::toPixel(prevAdvanceFP + kernFP);       // snap 12.4 fixed-point to nearest pixel
    }

    const EpdGlyph* glyph = font.getGlyph(cp, style);

    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;  // 12.4 fixed-point

    renderCharImpl<TextRotation::Rotated90CW>(*this, renderMode, font, cp, x, lastBaseY, black, style);
    prevCp = cp;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return frameBuffer; }

size_t GfxRenderer::getBufferSize() const { return frameBufferSize; }

// unused
// void GfxRenderer::grayscaleRevert() const { display.grayscaleRevert(); }

void GfxRenderer::displayGrayscaleBase(HalDisplay::RefreshMode fallback) const {
  if (darkMode_) invertScreen();
  display.displayGrayscaleBase(fallback, fadingFix);
  if (darkMode_) invertScreen();
}

void GfxRenderer::preconditionGrayscale() const { display.preconditionGrayscale(); }

void GfxRenderer::preconditionGrayscale(int x, int y, int w, int h) const {
  if (w <= 0 || h <= 0) return;
  // Rotate the logical rect's opposite corners to physical panel coords; the
  // physical bbox stays axis-aligned for all four orientations.
  int ax, ay, bx, by;
  rotateCoordinates(orientation, x, y, &ax, &ay, panelWidth, panelHeight);
  rotateCoordinates(orientation, x + w - 1, y + h - 1, &bx, &by, panelWidth, panelHeight);
  int x0 = ax < bx ? ax : bx, x1 = ax > bx ? ax : bx;
  int y0 = ay < by ? ay : by, y1 = ay > by ? ay : by;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= panelWidth) x1 = panelWidth - 1;
  if (y1 >= panelHeight) y1 = panelHeight - 1;
  if (x1 < x0 || y1 < y0) return;
  display.preconditionGrayscale(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0),
                                static_cast<uint16_t>(x1 - x0 + 1), static_cast<uint16_t>(y1 - y0 + 1));
}

void GfxRenderer::copyGrayscaleLsbBuffers() const {
  if (darkMode_) invertScreen();
  display.copyGrayscaleLsbBuffers(frameBuffer);
  if (darkMode_) invertScreen();
}

void GfxRenderer::copyGrayscaleMsbBuffers() const {
  if (darkMode_) invertScreen();
  display.copyGrayscaleMsbBuffers(frameBuffer);
  if (darkMode_) invertScreen();
}

void GfxRenderer::displayGrayBuffer() const { display.displayGrayBuffer(fadingFix); }

void GfxRenderer::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const {
  // Guard the uint16_t casts below: a negative would wrap to a huge length.
  assert(yStart >= 0 && numRows > 0 && yStart <= static_cast<int>(panelHeight) - numRows);
  if (darkMode_) {
    // Invert the strip for the push, restore after -- keeps the caller's
    // scratch contract side-effect-free (see setDarkMode). Strip is ~8KB.
    auto* mutableScratch = const_cast<uint8_t*>(scratch);
    const size_t stripBytes = static_cast<size_t>(numRows) * panelWidthBytes;
    for (size_t i = 0; i < stripBytes; i++) mutableScratch[i] = ~mutableScratch[i];
    display.writeGrayscalePlaneStrip(lsbPlane, scratch, static_cast<uint16_t>(yStart), static_cast<uint16_t>(numRows));
    for (size_t i = 0; i < stripBytes; i++) mutableScratch[i] = ~mutableScratch[i];
    return;
  }
  display.writeGrayscalePlaneStrip(lsbPlane, scratch, static_cast<uint16_t>(yStart), static_cast<uint16_t>(numRows));
}

bool GfxRenderer::supportsStripGrayscale() const { return display.supportsStripGrayscale(); }

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing 48KB of contiguous memory.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  // Allocate and copy each chunk
  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk", i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(chunkSize));

    if (!bwBufferChunks[i]) {
      LOG_ERR("GFX", "!! Failed to allocate BW buffer chunk %zu (%zu bytes)", i, chunkSize);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, chunkSize);
  }

  LOG_DBG("GFX", "Stored BW buffer in %zu chunks (%zu bytes each)", bwBufferChunks.size(), BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer() {
  // Check if all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    return;
  }

  for (size_t i = 0; i < bwBufferChunks.size(); i++) {
    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BW_BUFFER_CHUNK_SIZE, static_cast<size_t>(frameBufferSize - offset));
    memcpy(frameBuffer + offset, bwBufferChunks[i], chunkSize);
  }

  display.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  LOG_DBG("GFX", "Restored and freed BW buffer chunks");
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  if (frameBuffer) {
    // Controller RAM must hold what the panel SHOWS (the inverted frame in
    // dark mode) or the next differential refresh diffs against the wrong
    // baseline and ghosts every changed pixel.
    if (darkMode_) invertScreen();
    display.cleanupGrayscaleBuffers(frameBuffer);
    if (darkMode_) invertScreen();
  }
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}
