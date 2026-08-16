#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  // Release every rebuildable SD-font cache (mini glyph/kern arenas, kern/lig
  // class tables, overflow rings, advance tables) while keeping the fonts
  // loaded. Everything faults back in on demand. For heap-critical transitions
  // (e.g. web-server + WiFi startup); see SdCardFont::releaseResidentCaches().
  void releaseSdFontCaches();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);
  // Arabic body text renders from the RESOLVED Arabic font (not the caller's
  // fontId), with codepoints already shaped into presentation forms -- record
  // that stream separately so the page prewarm also fills a slot for the
  // Arabic font. Without this every Arabic glyph draw went through the
  // FontDecompressor hot-group fallback (repeated group decompression), which
  // made vocalized pages (the Quran) noticeably slow to turn.
  //
  // Keyed per fontId (not one shared buffer): a single Quran page legitimately
  // mixes several distinct Arabic fonts -- the reading font for ayah body text,
  // plus the surah banner's own dedicated calligraphy and caption-label fonts.
  // An earlier single-accumulator version appended every font's shaped text
  // into one buffer and prewarmed only the FIRST font recorded (typically the
  // banner, since it's drawn before the body) -- so the body text's glyphs,
  // absent from that tiny banner font, were silently skipped by prewarm and
  // every one of them fell through to the slow per-glyph decompression path on
  // the real render pass (real-device logs: 7-9s per page turn, 229 misses vs
  // 64 hits, pages left blank where the render never caught up). Recording
  // each font's text under its own key and prewarming every key at
  // endScanAndPrewarm() fixes this without reintroducing the "single global
  // font" assumption that broke as soon as a page could use more than one.
  void recordArabicText(const char* shapedUtf8, int fontId);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // Diagnostics only, set by endScanAndPrewarm(): real-device logs showed
  // prewarm_glyphs staying at 0 for the Quran even after fixing the scanText_.empty()
  // early return (which was skipping the Arabic prewarmCache() call entirely) --
  // meaning either the scan pass still isn't capturing the page's Arabic text, or
  // prewarmCache() is resolving the Arabic font id to something other than the
  // compressed-font path FontDecompressor::Stats tracks. These say which.
  //
  // Now that a page can prewarm multiple distinct Arabic fonts (see
  // recordArabicText()'s comment), these single-value fields describe the
  // PRIMARY font -- whichever accumulated the most text this page (almost
  // always the reading font, since body text dwarfs a banner/label font's
  // handful of glyphs) -- not literally "the only font". getLastArabicPrewarmFontCount()
  // says how many distinct fonts were actually prewarmed.
  enum class LastPrewarmPath : uint8_t { NotAttempted, NoFontFound, SdCardFont, Compressed };
  LastPrewarmPath getLastArabicPrewarmPath() const { return lastArabicPrewarmPath_; }
  int getLastArabicPrewarmFontId() const { return lastArabicPrewarmFontId_; }
  size_t getLastArabicScanTextBytes() const { return lastArabicScanTextBytes_; }
  size_t getLastArabicPrewarmFontCount() const { return lastArabicPrewarmFontCount_; }

  // Diagnostics only, called from GfxRenderer::drawArabicText() at every call made
  // while isScanning() is true -- BEFORE that function's own early return if the
  // resolved Arabic font isn't in fontMap. scan_bytes/path above showed prewarm never
  // captures the Quran's Arabic text at all (0 bytes, every single page), which is
  // only possible if either drawArabicText() is never entered during the scan pass,
  // or it's entered but exits before ever reaching recordArabicText() -- this counter
  // pair distinguishes the two: entries vs. entries where the font lookup failed.
  // Reset by PrewarmScope's constructor, same as the other per-page scan state.
  void noteArabicScanEntry(bool fontFound, int resolvedFontId);
  uint32_t getArabicScanEntries() const { return arabicScanEntries_; }
  uint32_t getArabicScanFontMissing() const { return arabicScanFontMissing_; }
  int getArabicScanLastResolvedFontId() const { return arabicScanLastResolvedFontId_; }

  // Diagnostics only: raw peek at the accumulator(s) recordArabicText() writes to,
  // taken by the caller immediately after the scan-pass render call returns and
  // BEFORE endScanAndPrewarm() reads/clears them. entries=71 font_missing=0 (proving
  // recordArabicText() must have run) alongside scan_bytes=0 (what endScanAndPrewarm
  // saw moments later) is a contradiction under a single-threaded read of this code --
  // this closes that gap by showing the true accumulator state at the earliest
  // possible point, before anything else has a chance to touch it. Reports totals
  // across every font's buffer (size) and the id of whichever buffer is currently
  // largest (font) -- same "primary font" convention as the post-prewarm getters
  // above, now that a page can have more than one.
  size_t peekScanArabicTextSize() const;
  int peekScanArabicFontId() const;

  // Diagnostics only: raw SdCardFont::Stats for the given font id, filled via
  // out-params instead of returning SdCardFont::Stats directly so this header
  // doesn't need SdCardFont's full definition (it's forward-declared above).
  // All-zero output means fontId isn't a currently-loaded SD-card font. Added
  // because logSlowPageTurn()'s existing stats section reads
  // getDecompressor()->getStats() unconditionally -- meaningless leftover
  // numbers on a path=sd turn, since that's the FLASH font's stats, not the
  // SD card read that's actually on the critical path.
  //
  // prewarmTotalMs is SdCardFont::prewarm()'s own outer timer (codepoint
  // dedup/sort + Storage.openFileForRead() + the seek/read loop + any
  // first-time kern/ligature table load); sdReadTimeMs only covers the
  // seek/read loop, timed AFTER the file is already open. The gap between the
  // two is exactly the part no other stat currently surfaces -- if it's large,
  // the bottleneck is the SD filesystem open/first-touch, not glyph data
  // volume.
  void getSdFontDiagStats(int fontId, uint32_t& prewarmTotalMs, uint32_t& sdReadTimeMs, uint32_t& seekCount,
                          uint32_t& uniqueGlyphs, uint32_t& bitmapBytes) const;

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;
  std::string scanText_;
  uint32_t scanStyleCounts_[4] = {};
  int scanFontId_ = -1;
  // One accumulator per Arabic font id seen this page -- see recordArabicText()'s
  // comment for why a single shared buffer/font-id pair isn't enough.
  std::map<int, std::string> scanArabicTextByFont_;

  LastPrewarmPath lastArabicPrewarmPath_ = LastPrewarmPath::NotAttempted;
  int lastArabicPrewarmFontId_ = -1;
  size_t lastArabicScanTextBytes_ = 0;
  size_t lastArabicPrewarmFontCount_ = 0;

  uint32_t arabicScanEntries_ = 0;
  uint32_t arabicScanFontMissing_ = 0;
  int arabicScanLastResolvedFontId_ = -1;
};
