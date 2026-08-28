#pragma once

#include <Epub/Page.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"

class DictionaryWordSelectActivity final : public Activity {
 public:
  // `initialQuery` non-empty means the word did not come off the page: the reader's
  // "Type a word" row collected it on the keyboard and handed it over. It is looked
  // up once, on the first loop, and the screen then behaves as normal word select --
  // so a typed lookup lands the user on the page they were reading rather than back
  // in a menu, and a second word can be picked by hand without reopening anything.
  DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Page> page,
                               int readerFontId, int marginLeft, int marginTop, std::string initialQuery = "")
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        readerFontId(readerFontId),
        marginLeft(marginLeft),
        marginTop(marginTop),
        initialQuery(std::move(initialQuery)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  struct WordInfo {
    std::string text;
    std::string lookupText;
    int16_t screenX = 0;
    int16_t screenY = 0;
    int16_t width = 0;
    int16_t row = 0;
    int continuationIndex = -1;
    int continuationOf = -1;
    // Extra pixels TextBlock's real render inserted as a tatweel/kashida glyph
    // to justify this word (0 for most words). Selection highlight/redraw must
    // use the same value the real paragraph draw used (see drawSelectionHighlight),
    // or the box undershoots the actual painted ink and leaves a sliver of the
    // original glyphs showing through -- most visible on the word's lead
    // letters for RTL text, looking like a duplicated first letter.
    uint16_t kashidaExtraPx = 0;
  };

  struct Row {
    int16_t y = 0;
    std::vector<int> wordIndices;
  };

  struct SelectionRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };

  struct SelectionRegionCache {
    SelectionRect rect;
    uint8_t* buffer = nullptr;
    size_t capacity = 0;
    size_t size = 0;
    bool stored = false;
  };

  static constexpr size_t MAX_SELECTION_REGIONS = 2;

  std::shared_ptr<Page> page;
  int readerFontId = 0;
  int marginLeft = 0;
  int marginTop = 0;
  // Cleared as it is consumed, so the lookup fires once rather than on every loop
  // and on every return from the definition screen.
  std::string initialQuery;
  std::vector<WordInfo> words;
  std::vector<Row> rows;
  int currentRow = 0;
  int currentWordInRow = 0;
  SelectionRegionCache selectionRegions[MAX_SELECTION_REGIONS];
  size_t selectionRegionCount = 0;

  void extractWords();
  void prepareReaderFontMetrics();
  int measureWordWidth(const char* text) const;
  // readerFontId is always the Latin reading font (Bitter/Lexend Deca), even for
  // an Arabic book -- GfxRenderer::drawText resolves Arabic text to a taller
  // Arabic font internally (see ArabicFontSystem), so a highlight/region box
  // sized off readerFontId's own line height clips Arabic glyphs whenever the
  // resolved Arabic font's ascender+descender exceeds it (worst with Cairo,
  // whose real metrics run ~1.87x em vs Bitter's ~1.0x). Mirrors
  // ChapterHtmlSlimParser::computeLineHeight's max() approach for page layout.
  int lineHeightForWord(const WordInfo& word) const;
  void mergeHyphenatedWords();
  void moveRow(int delta);
  void moveWord(int delta);
  void lookupSelectedWord();
  // Everything after the query is known: the prepare-progress popup, the index
  // lookup, and pushing the definition, the suggestion list or the failure popup.
  void lookupQuery(const std::string& query);
  void updateSelectionHighlight();
  bool redrawSelectionFast();
  void prewarmCurrentSelectionText() const;
  size_t collectSelectionRects(SelectionRect* rects, size_t maxRects) const;
  bool storeSelectionBaseRegions();
  bool restoreSelectionBaseRegions() const;
  void invalidateSelectionRegionCache();
  void freeSelectionRegionCache();
  void drawSelectionHighlight();
};
