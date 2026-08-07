#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

namespace BidiUtils {
// Paragraph base direction for the Unicode BiDi algorithm (UAX#9).
// AUTO: scan text for first strong directional character (P2/P3 rules)
// LTR:  force left-to-right paragraph embedding level
// RTL:  force right-to-left paragraph embedding level
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

namespace ArabicTextMarkers {
// True if `text` is exactly the Bismillah marker sequence drawn by
// GfxRenderer::drawArabicText's dedicated Bismillah branch. Exposed so
// ParsedText::extractLine can force center alignment on the Bismillah's own
// paragraph regardless of the reader's paragraph-alignment setting -- every
// printed Mushaf centers it, and CSS text-align alone can't win that fight
// since BlockStyle::fromCssStyle lets the user's setting override the book's
// CSS unless "Book's Style" is explicitly selected (see its comment).
bool isBismillah(const char* text);
}  // namespace ArabicTextMarkers

class FontCacheManager;
class SdCardFont;

#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Bitmap.h"

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory

  HalDisplay& display;
  RenderMode renderMode;
  Orientation orientation;
  bool fadingFix;
  bool darkMode_ = false;
  uint8_t* frameBuffer = nullptr;
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  std::vector<uint8_t*> bwBufferChunks;
  std::map<int, EpdFontFamily> fontMap;
  int arabicFontId_ = 0;  // 0 = no Arabic font loaded; see setArabicFontId()
  // Per-caller-fontId Arabic font overrides (e.g. SMALL_FONT_ID -> the 8pt Arabic
  // font), so Arabic text renders at the same size/baseline as whatever Latin font
  // the caller requested instead of always falling back to arabicFontId_'s single
  // fixed size. See setArabicFontIdForFontId()/resolveArabicFontId().
  std::map<int, int> arabicFontIdByFontId_;
  // Font to source Arabic-Indic digit glyphs (U+0660-0669) from for the code-drawn
  // ayah-end marker in drawArabicText/getArabicTextWidth, INSTEAD of whatever the
  // active reading font is. 0 = none set (falls back to the active font). See
  // setArabicDigitFallbackFontId() for why this needs to be independent of
  // arabicFontId_.
  int arabicDigitFallbackFontId_ = 0;
  // Font to source the real Bismillah ligature glyph (U+FDFD) from, for the
  // Bismillah marker in drawArabicText/getArabicTextWidth -- UthmanicHafs itself
  // lacks this glyph (see QuranCommon's font-registration comment in main.cpp).
  // 0 = none set (marker falls through to the plain-text path, which has no
  // glyph for U+FDFD and silently renders nothing -- matches the "missing glyph"
  // behavior used everywhere else in this file rather than special-casing it).
  int bismillahFontId_ = 0;
  // Font to source the calligraphic surah-name glyphs from, for the surah-banner
  // marker in drawArabicText/getArabicTextWidth -- surah-name-v4.ttf has no cmap
  // entries at all (see SurahNameV4's NOTICE.md), so no reading font could ever
  // supply these regardless of which one is active. 0 = none set (marker falls
  // through to the plain-text path, same missing-glyph behavior as bismillahFontId_).
  int surahBannerFontId_ = 0;
  // Font to source the surah banner's two small caption labels (ayah count,
  // revelation order) from, deliberately smaller/independent of both the
  // active reading font and surahBannerFontId_ -- a genuinely small font
  // rasterized at its own size, not the reading font runtime-scaled down (that
  // was tried first: renderCharScaled's half-scale rounding accumulates enough
  // error across a whole shaped, letter-joining word to visibly overlap
  // strokes -- fine for the isolated 1-3 digit runs it was designed for
  // elsewhere in this file, not for a multi-letter word). 0 = none set (falls
  // back to the resolved reading font, same as before this existed).
  int surahBannerLabelFontId_ = 0;
  // fontIds (keys of arabicFontIdByFontId_) whose Arabic text should sit on the
  // REQUESTED Latin font's baseline (baseline = y + Latin ascender) instead of the
  // Arabic font's own, much taller, nominal ascender. The Noto Arabic fonts reserve
  // enormous headroom above baseline for stacked diacritics (10pt: ascender 29px vs
  // the UI font's 21px at 10pt -- Inter; was Ubuntu's 20px), so anchoring by the
  // Arabic ascender pushes every Arabic string ~8px lower than the Latin text the
  // fixed UI geometry (30px list rows, 40px button hints, 45px header) was sized
  // for -- clipping the bottom of the glyphs. Plain, undiacritized UI labels never
  // use that headroom (measured worst-case ink above baseline across all shaped
  // presentation forms at 10pt: 21px, exactly at Inter's own 21px ascender; was
  // barely above Ubuntu's 20px), so sharing the Latin baseline both fits the
  // existing geometry and vertically aligns mixed Arabic/Latin strings.
  // Reading-text mappings deliberately stay unmatched: EPUB body rows are already
  // sized for the full Arabic line height and real book text does carry
  // diacritics that need the taller headroom.
  std::set<int> arabicBaselineMatchFontIds_;
  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // Tiled grayscale strip target. When active, drawPixel()/clearScreen()
  // operate on a caller-owned scratch holding one horizontal band of physical
  // rows [_stripY0, _stripY0 + _stripRows) (panelWidthBytes wide) instead of
  // the shared framebuffer, clipping pixels outside the band. Lets grayscale
  // planes render band-by-band straight to the controller without destroying
  // the BW framebuffer (no storeBwBuffer). Mutable because the render path is
  // const. See beginStripTarget()/endStripTarget().
  mutable uint8_t* _stripBuf = nullptr;
  mutable int _stripY0 = 0;
  mutable int _stripRows = 0;
  mutable bool _stripActive = false;

  // Shared implementation of drawCenteredTextWrapped() and measureWrappedTextHeight():
  // one greedy word-wrap loop, with drawing switched off for the measuring caller, so the
  // measured height and the drawn height cannot drift apart.
  int layoutCenteredTextWrapped(int fontId, int y, int maxWidth, const char* text, int maxLines, bool black,
                                EpdFontFamily::Style style, bool draw) const;
  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  // Arabic font to actually use for a caller-requested fontId: the specific
  // per-fontId override if one was registered via setArabicFontIdForFontId(),
  // otherwise the single default arabicFontId_.
  int resolveArabicFontId(int fontId) const {
    const auto it = arabicFontIdByFontId_.find(fontId);
    return it != arabicFontIdByFontId_.end() ? it->second : arabicFontId_;
  }
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Byte-aligned, orientation-specialized rectangle fill. Rotates the rect's
  // two opposing corners into physical-framebuffer space once, then walks each
  // physical row with head-mask / middle memset / tail-mask byte writes — no
  // per-pixel rotation, no per-pixel RMW.
  template <Color color>
  void fillRectImpl(int x, int y, int width, int height) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay), renderMode(BW), orientation(Portrait), fadingFix(false) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  // Public view of resolveArabicFontId() for cache-key purposes: the Arabic
  // font that Arabic text drawn with this fontId actually renders in. Section
  // layout caches must include it -- changing the Arabic reading font/size
  // otherwise leaves the Latin fontId untouched and stale layouts keep loading.
  int getArabicFontIdFor(const int fontId) const { return resolveArabicFontId(fontId); }

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Clears both the flash-font map and any SD-font registration for fontId.
  // Coupled to avoid dangling SdCardFont* in sdCardFonts_ when callers free
  // the underlying SdCardFont and forget the SD-side unregister.
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    sdCardFonts_.erase(fontId);
  }
  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  bool isFontCacheScanning() const;
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  // Default Arabic font (0 = none loaded), used for any fontId with no specific
  // override registered via setArabicFontIdForFontId() below. Set by ArabicFontSystem
  // once an Arabic font is loaded via insertFont(); drawArabicText/getArabicTextWidth
  // render from this font for callers with no per-fontId mapping, since none of the
  // built-in Latin fonts carry Arabic glyphs.
  void setArabicFontId(int fontId) { arabicFontId_ = fontId; }
  // Font to source Arabic-Indic digit glyphs from for the ayah-end marker, always,
  // regardless of which font is active for reading. Reading fonts are free to omit
  // decorative/rare glyphs -- a previous built-in reading face lacked Arabic-Indic
  // digits entirely (real-device evidence: the marker rendered as an empty numberless
  // circle) -- but the marker must look the same on every single page no matter which
  // reading font a book requests, so its digits deliberately don't come from
  // arabicFontId_/resolveArabicFontId() at all. Callers should pass a font guaranteed
  // to always be registered (e.g. a built-in UI-tier Arabic font set once at boot),
  // not something that depends on user configuration.
  void setArabicDigitFallbackFontId(int fontId) { arabicDigitFallbackFontId_ = fontId; }
  // Font to source the Bismillah ligature glyph (U+FDFD) from. Same "independent of
  // arabicFontId_/whatever reading font is active" reasoning as
  // setArabicDigitFallbackFontId() above -- the Bismillah must render as the real
  // ligature glyph regardless of which font the current book/Quran is using to read.
  void setBismillahFontId(int fontId) { bismillahFontId_ = fontId; }
  // Font to source the calligraphic surah-name glyphs from. Same "independent of
  // arabicFontId_" reasoning as setBismillahFontId() above.
  void setSurahBannerFontId(int fontId) { surahBannerFontId_ = fontId; }
  // Font to source the surah banner's small caption labels from. Same
  // "independent of arabicFontId_" reasoning as setBismillahFontId() above.
  void setSurahBannerLabelFontId(int fontId) { surahBannerLabelFontId_ = fontId; }
  // Registers an Arabic font to use specifically when the caller requests `fontId`
  // (e.g. SMALL_FONT_ID -> the 8pt Arabic font), so Arabic text matches the size and
  // baseline of whatever Latin font the caller asked for instead of always rendering
  // at arabicFontId_'s single fixed size. ArabicFontSystem registers these for the
  // built-in default; cleared when an SD-card Arabic font override is active so that
  // override applies uniformly via arabicFontId_ instead.
  // matchLatinBaseline=true additionally anchors the Arabic text on the requested
  // Latin font's baseline so it fits fixed Latin-sized UI geometry -- see
  // arabicBaselineMatchFontIds_ above. Use for UI-font mappings only, never for
  // reading-text mappings.
  void setArabicFontIdForFontId(int fontId, int arabicFontId, bool matchLatinBaseline = false) {
    arabicFontIdByFontId_[fontId] = arabicFontId;
    if (matchLatinBaseline) {
      arabicBaselineMatchFontIds_.insert(fontId);
    } else {
      arabicBaselineMatchFontIds_.erase(fontId);
    }
  }
  void clearArabicFontIdMappings() {
    arabicFontIdByFontId_.clear();
    arabicBaselineMatchFontIds_.clear();
  }
  // Public wrapper so callers (e.g. a Settings preview pane) can see which Arabic
  // font a given caller fontId currently resolves to, without duplicating the
  // per-fontId-override-vs-catch-all lookup logic.
  int getResolvedArabicFontId(int fontId) const { return resolveArabicFontId(fontId); }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }
  void unregisterSdCardFont(int fontId) { removeFont(fontId); }
  void clearSdCardFonts() { sdCardFonts_.clear(); }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }
  // Ensure SD card font glyph data is loaded for the given text. Called from layout code
  // (which holds a const GfxRenderer&) before measuring word widths. Safe to call on non-SD fonts (no-op).
  // styleMask: bitmask of styles to prepare (bit 0=regular, 1=bold, 2=italic, 3=bold-italic).
  void ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;
  // words: deque, not vector -- matches ParsedText::words (see its declaration for why).
  void ensureSdCardFontReady(int fontId, const std::deque<std::string>& words, bool includeHyphen,
                             uint8_t styleMask = 0x0F) const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }
  // Global dark mode: the framebuffer (and grayscale planes) are inverted at
  // the moment pixels are pushed to the panel, then restored -- so EVERY
  // consumer (reader incl. Arabic shaping/markers, games, home theme, status
  // bar) flips with zero per-activity code, and anything that reads the
  // framebuffer (screenshots, sleep-frame save, region cache, grayscale
  // re-sync) still sees normal polarity. Grays invert symmetrically (2-bit
  // planes are bitwise-NOTed, so AA edges stay correct around white-on-black
  // text); photos render as negatives -- accepted v1 tradeoff.
  void setDarkMode(const bool enabled) { darkMode_ = enabled; }
  bool isDarkMode() const { return darkMode_; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  // forceCleanBaseOnHalf: see HalDisplay::displayBuffer's own comment. Defaults
  // to true (existing behavior); the reader's periodic ghost-cleanup refresh
  // is the one caller that passes false.
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH,
                     bool forceCleanBaseOnHalf = true) const;
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Tiled grayscale strip target. While active, drawPixel() and clearScreen()
  // operate on `scratch` (panelWidthBytes * stripRows bytes, holding physical
  // rows [stripY0, stripY0 + stripRows)) instead of the framebuffer; pixels
  // whose physical row falls outside the band are clipped. The clip is applied
  // after the orientation rotate, so it is orientation-agnostic. Used to render
  // grayscale planes band-by-band without a full second buffer.
  void beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const;
  void endStripTarget() const;

  // Band culling for tiled grayscale. Takes a glyph bounding box in logical
  // screen coords and returns false only when a strip is active AND the box's
  // physical y-extent lies entirely outside the active band, letting callers
  // skip an expensive bitmap decode. Returns true when no strip is active.
  // Corners are rotated to physical, so it is orientation-aware.
  bool glyphIntersectsStrip(int x0, int y0, int x1, int y1) const;

  // Active pixel-write target for raw writers (DirectPixelWriter) that bypass
  // drawPixel for speed. When a strip target is active these return the band
  // scratch plus its physical-row origin and extent; otherwise the full
  // framebuffer ([0, panelHeight)). Writers subtract the origin and clip to the
  // extent, so they honor tiled-grayscale banding without per-pixel method calls.
  uint8_t* getWriteTarget() const { return _stripActive ? _stripBuf : frameBuffer; }
  int getWriteOriginY() const { return _stripActive ? _stripY0 : 0; }
  int getWriteRows() const { return _stripActive ? _stripRows : panelHeight; }

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color = Color::White) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  void drawIcon(const uint8_t bitmap[], int x, int y, int size) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0,
                  float cropY = 0) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                        BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  /// Greedy word-wrap of `text` to `maxWidth`, drawn centered, one line per row from `y`
  /// down. Returns the total height consumed, so a caller can lay out whatever follows
  /// without assuming a line count.
  ///
  /// The wrapping counterpart to truncatedText(): use that when a label must stay on one
  /// row (list rows, grid captions), this when losing the tail would lose meaning -- a
  /// sentence telling the user what went wrong and what to do about it. drawCenteredText()
  /// alone does neither and simply overruns the panel, which clips the text at *both*
  /// edges once it is wider than the screen, since the draw is centered.
  ///
  /// Breaks on ASCII spaces only, so it can never split a UTF-8 sequence. A single word
  /// too wide to fit is ellipsized via truncatedText() rather than dropped. On reaching
  /// `maxLines` the remaining text is ellipsized into the final line, so overflow is
  /// always visible as "..." instead of silently vanishing.
  int drawCenteredTextWrapped(int fontId, int y, int maxWidth, const char* text, int maxLines, bool black = true,
                              EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// The height drawCenteredTextWrapped() would consume, without drawing anything. For
  /// callers that must vertically center a wrapped block: the block's height is not known
  /// until the wrap is computed, so the alternative is drawing it twice.
  ///
  /// Shares the wrap loop with the draw path, so the two can never disagree about where
  /// the line breaks fall.
  int measureWrappedTextHeight(int fontId, int maxWidth, const char* text, int maxLines,
                               EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// kashidaExtraPx: extra width (already floored to a whole tatweel-glyph multiple by
  /// the caller) to insert via kashida when this call ends up on the Arabic path --
  /// see ParsedText::computeJustifyPlan. Ignored on the non-Arabic path; Latin
  /// justification keeps using inter-word gap stretching exclusively.
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int kashidaExtraPx = 0) const;
  /// Draws a single line within the box [x, x+width): left-aligned as normal, but
  /// right-aligned when the text is Arabic (ScriptDetector::containsArabic), matching
  /// the natural reading direction instead of always anchoring at the box's left edge.
  /// For titles/labels drawn into a fixed-width row or grid cell (list rows, cover grid
  /// captions) where the caller doesn't otherwise track alignment itself.
  void drawTextInWidth(int fontId, int x, int y, int width, const char* text, bool black = true,
                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Arabic-script text path: contextual shaping (isolated/initial/medial/final forms) +
  /// Lam-Alef ligatures via ArabicShaper, then rendered in the already-visual-order the
  /// shaper returns (bypasses MiniBidi entirely -- shaping already reorders). Glyphs come
  /// from a dedicated Arabic font (ArabicFontSystem), not the fontId passed in, since none
  /// of the built-in fonts carry Arabic glyphs. drawText/getTextWidth dispatch here
  /// automatically via ScriptDetector::containsArabic() -- not normally called directly.
  int getArabicTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                         int kashidaExtraPx = 0) const;
  void drawArabicText(int fontId, int x, int y, const char* text, bool black = true,
                      EpdFontFamily::Style style = EpdFontFamily::REGULAR, int kashidaExtraPx = 0) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// The active Arabic font's own U+0640 TATWEEL glyph width in pixels, or 0 if it has
  /// none. Layout-time query for kashida justification (ParsedText::computeJustifyPlan).
  int getKashidaGlyphWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Does this single word contain a legal kashida (tatweel) insertion point in the
  /// active Arabic font? See ArabicShaper::hasKashidaPoint.
  bool wordHasKashidaPoint(int fontId, const char* word, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  /// Returns the kerning adjustment between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  RenderMode getRenderMode() const { return renderMode; }
  // Grayscale preconditioning settle pass (no-op on X4). The rect overload
  // takes the gray region in LOGICAL screen coordinates and rotates it to the
  // panel; the no-arg overload settles the full frame. Call after the BW base
  // frame is displayed and before the grayscale planes are written.
  void preconditionGrayscale() const;
  void preconditionGrayscale(int x, int y, int w, int h) const;
  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows (X3: OEM differential base waveform; others: plain display with
  // `fallback`).
  void displayGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;

  // Tiled grayscale (X4): stream one band of a plane straight to controller RAM
  // from `scratch` (panelWidthBytes * numRows, physical rows [yStart, yStart+
  // numRows)), bypassing the framebuffer. supportsStripGrayscale() gates use.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const;
  bool supportsStripGrayscale() const;
  bool storeBwBuffer();    // Returns true if buffer was stored successfully
  void restoreBwBuffer();  // Restore and free the stored buffer
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache: take a logical (orientation-aware) rect, hit the framebuffer
  // bytes that the rect can have touched, and pump them in or out of a caller-
  // supplied buffer. Used by HomeActivity to snapshot just the cover tile
  // (~16 KB in Portrait) instead of cloning the entire 48 KB framebuffer.
  //
  // getRegionByteSize: required buffer length for the rect at current orientation.
  // copyRegionToBuffer / copyBufferToRegion: false if `bufSize` is smaller than that.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};
