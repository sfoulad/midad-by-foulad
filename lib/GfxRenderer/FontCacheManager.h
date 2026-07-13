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
  void recordArabicText(const char* shapedUtf8, int fontId);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // Diagnostics only, set by endScanAndPrewarm(): real-device logs showed
  // prewarm_glyphs staying at 0 for the Quran even after fixing the scanText_.empty()
  // early return (which was skipping the Arabic prewarmCache() call entirely) --
  // meaning either the scan pass still isn't capturing the page's Arabic text, or
  // prewarmCache() is resolving the Arabic font id to something other than the
  // compressed-font path FontDecompressor::Stats tracks. These say which.
  enum class LastPrewarmPath : uint8_t { NotAttempted, NoFontFound, SdCardFont, Compressed };
  LastPrewarmPath getLastArabicPrewarmPath() const { return lastArabicPrewarmPath_; }
  int getLastArabicPrewarmFontId() const { return lastArabicPrewarmFontId_; }
  size_t getLastArabicScanTextBytes() const { return lastArabicScanTextBytes_; }

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
  std::string scanArabicText_;
  int scanArabicFontId_ = -1;

  LastPrewarmPath lastArabicPrewarmPath_ = LastPrewarmPath::NotAttempted;
  int lastArabicPrewarmFontId_ = -1;
  size_t lastArabicScanTextBytes_ = 0;

  uint32_t arabicScanEntries_ = 0;
  uint32_t arabicScanFontMissing_ = 0;
  int arabicScanLastResolvedFontId_ = -1;
};
