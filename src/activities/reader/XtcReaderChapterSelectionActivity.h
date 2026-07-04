#pragma once
#include <Xtc.h>

#include <memory>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class XtcReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;
  ButtonNavigator buttonNavigator;
  uint32_t currentPage = 0;
  int selectorIndex = 0;

  int getPageItems() const;
  // Row height in pixels: Arabic chapter titles need noticeably more vertical space than
  // Latin ones (the Arabic font's ascender+descender is roughly double, so a Latin-sized
  // row clips Arabic glyph tops/tails). Checks whether any chapter title is Arabic and
  // sizes every row for the tallest font actually in use, so getPageItems()'s pagination
  // and render()'s highlight/text placement always agree on the same row height.
  int getRowHeight() const;
  int findChapterIndexForPage(uint32_t page) const;

 public:
  explicit XtcReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Xtc>& xtc, uint32_t currentPage)
      : Activity("XtcReaderChapterSelection", renderer, mappedInput), xtc(xtc), currentPage(currentPage) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
