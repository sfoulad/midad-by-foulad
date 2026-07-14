#pragma once

#include <InflateReader.h>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  // Ceiling on distinct glyphs/groups prewarmCache() can batch-decompress for one page.
  // Was 512/128 (fine for Latin scripts, where a page can't realistically use more than
  // a few dozen distinct glyphs) but far too low for fully-vocalized Arabic: each base
  // letter has multiple positional forms combined with multiple diacritics, so a dense
  // page can need well over a thousand distinct glyphs. Anything past the cap silently
  // fell back to per-glyph hot-group decompression *during* the render pass instead of
  // the batched prewarm pass -- real-device logs showed prewarm consistently fast
  // (12-25ms) while the render pass that followed it took 8-15 SECONDS on pages that
  // exceeded the old cap (see EpubReaderActivity's per-page-turn perf log). Raised with
  // real headroom now that these are heap-allocated (see prewarmCache()), not a fixed
  // stack array sized for the old, much smaller cap.
  static constexpr uint16_t MAX_PAGE_GLYPHS = 4096;
  static constexpr uint16_t MAX_PAGE_GROUPS = 512;
  // Originally 4 (one per font style: R/B/I/BI) on the assumption a page only ever
  // prewarms a single font's styles. A Quran surah-header page breaks that
  // assumption: FontCacheManager::recordArabicText() now keys its scan buffer per
  // Arabic font id (not one shared buffer -- see its own comment), and such a page
  // legitimately records 5 distinct fonts in one scan: the reading font (ayah body
  // text), the surah banner's calligraphy font, its caption-label font, the
  // ayah-marker digit-fallback font, and the Bismillah ligature's dedicated font.
  // Each currently only needs 1 slot (Arabic reading text is always REGULAR style),
  // so 4 slots meant the 5th font's prewarm call was silently rejected
  // ("All 4 page buffer slots full") and every glyph in that font fell back to the
  // slow per-glyph decompression path for the rest of that page's render. Raised
  // with a little headroom above the current worst case (5); each slot is just a
  // few pointers (see PageSlot below) until actually populated, so this costs
  // nothing until a page really uses that many fonts.
  static constexpr uint8_t MAX_PAGE_SLOTS = 8;

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffer + hot group).
  void clearCache();

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page buffer.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t hotGroupBytes = 0;    // current hot group allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
    // Real-device-only failure mode: a malloc() failure here (page slot buffer,
    // hot-group buffer, or glyph scratch buffer -- all under real memory pressure,
    // never reproduced against the simulator's effectively-unlimited heap) makes
    // getBitmap() return nullptr, which every caller (renderCharImpl and friends)
    // silently treats as "skip this glyph" -- the page keeps rendering, just with
    // that glyph invisible, no error surfaced anywhere the reader would see. A
    // failed malloc returns near-instantly, so this can happen on a page turn that
    // *isn't* slow -- unlike hits/misses/decompressTimeMs, this must be checked
    // independent of the SLOW_PAGE_TURN_MS gate that guards the rest of the
    // per-turn perf log line, or a fast-but-glyph-dropping turn leaves zero trace.
    uint32_t bitmapAllocFailures = 0;
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;

  // Hot group: last decompressed group (byte-aligned) for non-prewarmed fallback path.
  // Kept in byte-aligned format; individual glyphs are compacted on demand into hotGlyphBuf.
  // Nothrow high-water malloc buffers, NOT std::vector: getBitmap() runs on the render path,
  // and under -fno-exceptions a vector resize that hits OOM abort()s the firmware instead of
  // failing (field crash: hotGroup.resize() -> std::bad_alloc -> abort with ~11 KB free).
  // ensureCapacity() returns false on OOM so the caller can skip the glyph gracefully.
  const EpdFontData* hotGroupFont = nullptr;
  uint16_t hotGroupIndex = UINT16_MAX;
  uint8_t* hotGroup = nullptr;  // owned; freed in freeHotGroup()/dtor
  uint32_t hotGroupCapacity = 0;

  // Scratch buffer for compacting a single glyph from the hot group.
  // Valid until the next getBitmap() call. Same ownership/OOM contract as hotGroup.
  uint8_t* hotGlyphBuf = nullptr;
  uint32_t hotGlyphBufCapacity = 0;

  // Grow (never shrink) an owned buffer to at least `needed` bytes; false on OOM, buffer freed.
  static bool ensureCapacity(uint8_t*& buf, uint32_t& capacity, uint32_t needed);

  void freePageBuffer();
  void freeHotGroup();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);

  // Every glyph's byte-aligned offset within its own group, precomputed for the WHOLE
  // font in one pass and cached for the FontDecompressor's lifetime -- the offset only
  // depends on the font's static group/glyph layout, never on runtime state, so there's
  // no reason to recompute it per lookup. Replaces getAlignedOffset()'s per-call scan
  // (O(glyphIndex) for a frequency-grouped font) on the hot-group fallback path in
  // getBitmap(): a fully-vocalized Arabic page can miss the batched prewarm cache for
  // hundreds of distinct glyphs, and each miss re-scanning from glyph 0 dominated
  // render time on real hardware (13+ SECONDS per page turn, unmoved by raising the
  // prewarm cap -- the bottleneck was this per-glyph rescan, not prewarm coverage).
  bool ensureAlignedOffsetTable(const EpdFontData* fontData);
  const EpdFontData* alignedOffsetTableFont = nullptr;
  uint32_t* alignedOffsetTable = nullptr;  // indexed directly by glyphIndex; owned, freed in deinit()/dtor
  uint32_t alignedOffsetTableSize = 0;
};
