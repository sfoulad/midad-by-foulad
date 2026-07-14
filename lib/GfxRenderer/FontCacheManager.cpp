#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  scanText_ += text;
  if (scanFontId_ < 0) scanFontId_ = fontId;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cpCount = 0;
  while (*p) {
    if ((*p & 0xC0) != 0x80) cpCount++;
    p++;
  }
  scanStyleCounts_[baseStyle] += cpCount;
}

void FontCacheManager::recordArabicText(const char* shapedUtf8, int fontId) {
  scanArabicTextByFont_[fontId] += shapedUtf8;
}

namespace {
// Shared by peekScanArabicTextSize()/peekScanArabicFontId() and endScanAndPrewarm():
// total bytes across every font's buffer, plus the id of whichever buffer is
// currently largest (the "primary" font for single-value diagnostics fields).
void summarizeArabicScan(const std::map<int, std::string>& byFont, size_t& totalBytes, int& primaryFontId) {
  totalBytes = 0;
  primaryFontId = -1;
  size_t primaryBytes = 0;
  for (const auto& [fontId, text] : byFont) {
    totalBytes += text.size();
    if (text.size() > primaryBytes) {
      primaryBytes = text.size();
      primaryFontId = fontId;
    }
  }
}
}  // namespace

size_t FontCacheManager::peekScanArabicTextSize() const {
  size_t totalBytes;
  int primaryFontId;
  summarizeArabicScan(scanArabicTextByFont_, totalBytes, primaryFontId);
  return totalBytes;
}

int FontCacheManager::peekScanArabicFontId() const {
  size_t totalBytes;
  int primaryFontId;
  summarizeArabicScan(scanArabicTextByFont_, totalBytes, primaryFontId);
  return primaryFontId;
}

void FontCacheManager::noteArabicScanEntry(bool fontFound, int resolvedFontId) {
  arabicScanEntries_++;
  if (!fontFound) arabicScanFontMissing_++;
  arabicScanLastResolvedFontId_ = resolvedFontId;
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  manager_->scanText_.clear();
  manager_->scanText_.reserve(2048);  // Pre-allocate to avoid heap fragmentation from repeated concat
  manager_->scanArabicTextByFont_.clear();
  memset(manager_->scanStyleCounts_, 0, sizeof(manager_->scanStyleCounts_));
  manager_->scanFontId_ = -1;
  manager_->arabicScanEntries_ = 0;
  manager_->arabicScanFontMissing_ = 0;
  manager_->arabicScanLastResolvedFontId_ = -1;
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  // NOT scanText_.empty() alone: a page that's entirely Arabic (the Quran; any
  // fully-Arabic book with no incidental Latin digits/punctuation on the page) never
  // calls recordText(), so scanText_ stays empty even though scanArabicTextByFont_ is
  // full. The old check returned here before EVER reaching the Arabic prewarmCache()
  // call below -- meaning prewarm silently never ran at all for such a page, every
  // single glyph fell through to the slow per-glyph hot-group fallback for the entire
  // render, and every earlier fix to WHAT got recorded (or how fast a miss was
  // handled) was moot because prewarmCache() was never being invoked in the first
  // place. Real-device evidence: a mixed-script Arabic novel (incidental Latin
  // content keeps scanText_ non-empty) hit ~90%; the Quran (deliberately zero Latin
  // anywhere, including Arabic-Indic page/ayah numbers) hit ~15%, every page, no
  // matter which other fix landed.
  if (manager_->scanText_.empty() && manager_->scanArabicTextByFont_.empty()) return;

  // Build style bitmask from all styles that appeared during the scan
  uint8_t styleMask = 0;
  for (uint8_t i = 0; i < 4; i++) {
    if (manager_->scanStyleCounts_[i] > 0) styleMask |= (1 << i);
  }
  if (styleMask == 0) styleMask = 1;  // default to regular

  if (!manager_->scanText_.empty()) {
    manager_->prewarmCache(manager_->scanFontId_, manager_->scanText_.c_str(), styleMask);
  }
  if (!manager_->scanArabicTextByFont_.empty()) {
    size_t totalBytes;
    int primaryFontId;
    summarizeArabicScan(manager_->scanArabicTextByFont_, totalBytes, primaryFontId);
    manager_->lastArabicScanTextBytes_ = totalBytes;
    manager_->lastArabicPrewarmFontId_ = primaryFontId;
    manager_->lastArabicPrewarmFontCount_ = manager_->scanArabicTextByFont_.size();
    // NOT `primaryFontId >= 0`: SD-card font ids are FNV-1a hashes cast to `int`
    // (SdCardFontManager::computeFontId), which are legitimately negative about half
    // the time. That stray sign check meant every SD-card Arabic font whose hash came
    // out negative silently skipped this branch on every single page -- the
    // accumulator was full (see peekScanArabicTextSize() diagnostics: pre_bytes>0
    // every page) but this guard rejected it before prewarmCache() was ever called,
    // so the Quran (whose configured Arabic font hashed to a negative id) always fell
    // through to the slow per-glyph decompression path. A non-empty accumulator
    // already proves recordArabicText() ran at least once, which is all this needs to
    // know -- same check the Latin branch above already uses without a sign gate on
    // scanFontId_.
    if (manager_->sdCardFonts_.count(primaryFontId) > 0) {
      manager_->lastArabicPrewarmPath_ = LastPrewarmPath::SdCardFont;
    } else if (manager_->fontMap_.count(primaryFontId) > 0) {
      manager_->lastArabicPrewarmPath_ = LastPrewarmPath::Compressed;
    } else {
      manager_->lastArabicPrewarmPath_ = LastPrewarmPath::NoFontFound;
    }
    // Prewarm EVERY font that appeared during the scan, not just one -- a single
    // Quran page legitimately mixes several Arabic fonts (the reading font for ayah
    // body text, plus the surah banner's own dedicated calligraphy/label fonts).
    // Dumping every font's shaped text into one shared buffer and prewarming only
    // that buffer's (first- or largest-recorded) font meant glyphs belonging to the
    // OTHER fonts were silently "missing" from that one prewarm call and never got
    // cached -- see recordArabicText()'s comment for the real-device symptom this
    // caused (7-9s page turns, pages left blank mid-render). Arabic reading text is
    // always REGULAR style.
    //
    // Largest-text-first, not map (font id) order: prewarming now does up to 5x the
    // malloc/free churn per page it used to (one call per font instead of one call
    // total), which real-device logs showed fragmenting the heap badly enough that
    // by the time a LATER font's turn came up, its own page-buffer malloc -- and
    // then its hot-group fallback for nearly every glyph -- failed outright
    // (bitmap_fail almost exactly equal to misses, every turn, heap otherwise
    // stable around 75-80KB: the signature of "no contiguous block big enough",
    // not "actually out of memory"). The reading font's text dwarfs the banner/
    // label/digit fonts' combined total (thousands of bytes vs a few hundred), so
    // give it first crack at the least-fragmented heap this page's cycle will see;
    // if the small decorative fonts lose that race instead, a missed banner glyph
    // or ayah digit is far less noticeable than a paragraph of blank body text.
    std::vector<std::pair<int, const std::string*>> byFontDescending;
    byFontDescending.reserve(manager_->scanArabicTextByFont_.size());
    for (const auto& [fontId, text] : manager_->scanArabicTextByFont_) {
      byFontDescending.emplace_back(fontId, &text);
    }
    std::sort(byFontDescending.begin(), byFontDescending.end(),
              [](const auto& a, const auto& b) { return a.second->size() > b.second->size(); });
    for (const auto& [fontId, text] : byFontDescending) {
      manager_->prewarmCache(fontId, text->c_str(), 0x01);
    }
  } else {
    manager_->lastArabicPrewarmPath_ = LastPrewarmPath::NotAttempted;
    manager_->lastArabicScanTextBytes_ = 0;
    manager_->lastArabicPrewarmFontId_ = -1;
    manager_->lastArabicPrewarmFontCount_ = 0;
  }

  // Free scan string memory
  manager_->scanText_.clear();
  manager_->scanText_.shrink_to_fit();
  manager_->scanArabicTextByFont_.clear();
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scanText_ is empty)
    manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
