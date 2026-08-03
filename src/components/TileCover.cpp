#include "TileCover.h"

#include <algorithm>
#include <string>

#include "fontIds.h"

// "Cover" for synthetic feature tiles (Games, Tasbih) -- there's no real cover
// image (their paths are never real SD files), and the generic BookIcon
// placeholder used for every other missing-cover case made it look like a
// broken/unopened book rather than a deliberate feature tile (an earlier
// hand-drawn puzzle pictogram had the same problem: too easy to mistake for
// clutter at a glance). A solid black tile with the label centered reads
// instantly as its own distinct "book" in the grid, the way a plain black
// spine stands out on a shelf. Drawn with plain fillRect/drawText primitives,
// no bitmap asset needed.
void drawTileCover(GfxRenderer& renderer, int cellX, int cellY, int cellWidth, int cellHeight, const char* label) {
  renderer.fillRect(cellX, cellY, cellWidth, cellHeight, true);

  // Thin white inset frame -- a plain black rectangle reads as "missing
  // cover"; a bordered one reads as a designed cover. Inset (not flush)
  // so it doesn't fight the grid's own cell outline drawn over this by the
  // caller.
  const int inset = std::max(4, cellWidth / 20);
  const int frameThickness = std::max(1, cellWidth / 60);
  renderer.drawRect(cellX + inset, cellY + inset, cellWidth - 2 * inset, cellHeight - 2 * inset, frameThickness, false);

  // Largest available UI font, bold, white-on-black, centered in the cell.
  // Falls back a size down if the label would overflow the inset frame (long
  // translations, e.g. German/French labels).
  int fontId = UI_12_FONT_ID;
  int textWidth = renderer.getTextWidth(fontId, label, EpdFontFamily::BOLD);
  const int maxTextWidth = cellWidth - 4 * inset;
  if (textWidth > maxTextWidth) {
    fontId = UI_10_FONT_ID;
    textWidth = renderer.getTextWidth(fontId, label, EpdFontFamily::BOLD);
  }
  if (textWidth > maxTextWidth) {
    fontId = SMALL_FONT_ID;
    textWidth = renderer.getTextWidth(fontId, label, EpdFontFamily::BOLD);
  }
  // Shrinking twice is not enough for a long feed name -- clip what is left so the
  // label stays inside the frame instead of running past it.
  const std::string shown = renderer.truncatedText(fontId, label, maxTextWidth);
  textWidth = renderer.getTextWidth(fontId, shown.c_str(), EpdFontFamily::BOLD);
  const int textX = cellX + (cellWidth - textWidth) / 2;
  const int textY = cellY + (cellHeight - renderer.getLineHeight(fontId)) / 2;
  renderer.drawText(fontId, textX, textY, shown.c_str(), false, EpdFontFamily::BOLD);

  // A short rule above and below the label -- breaks up a plain word on a
  // plain field into something that looks like a designed logotype/badge
  // rather than a stray caption.
  const int ruleWidth = std::min(cellWidth - 6 * inset, textWidth + 4 * inset);
  const int ruleX = cellX + (cellWidth - ruleWidth) / 2;
  const int ruleGap = std::max(6, cellHeight / 18);
  renderer.fillRect(ruleX, textY - ruleGap, ruleWidth, frameThickness, false);
  renderer.fillRect(ruleX, textY + renderer.getLineHeight(fontId) + ruleGap - frameThickness, ruleWidth, frameThickness,
                    false);
}
