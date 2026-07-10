#include "FouladTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/stats.h"
#include "components/icons/transfer.h"
#include "fontIds.h"
#include "reading/ReadingStats.h"

namespace {
constexpr int kHeroCoverWidth = 175;
constexpr int kHeroGap = 16;         // gap between hero cover and the text column
constexpr int kSelectionBorder = 3;  // selection frame thickness around covers
constexpr int kRecentsGap = 12;      // gap between recents covers

const uint8_t* menuIconBitmap(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Stats:
      return StatsIcon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    default:
      return nullptr;
  }
}

// Draws a book cover (fit by height, crop horizontally like Lyra3Covers) or a
// placeholder box when there's no usable cover file.
void drawCoverAt(const GfxRenderer& renderer, const std::string& coverBmpPath, const int x, const int y, const int w,
                 const int h) {
  bool hasCover = false;
  if (!coverBmpPath.empty()) {
    const std::string path = UITheme::getCoverThumbPath(coverBmpPath, h);
    HalFile file;
    if (Storage.openFileForRead("HOME", path, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float tileRatio = static_cast<float>(w) / static_cast<float>(h);
        const float cropX = std::max(0.0f, 1.0f - (tileRatio / ratio));
        renderer.drawBitmap(bitmap, x, y, w, h, cropX);
        hasCover = true;
      }
    }
  }
  renderer.drawRect(x, y, w, h, true);
  if (!hasCover) {
    renderer.fillRect(x, y + h / 3, w, 2 * h / 3, true);
    renderer.drawIcon(CoverIcon, x + (w - 32) / 2, y + h / 6 - 16 + h / 3, 32);
  }
}
}  // namespace

void FouladTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                      const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                      bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const auto& m = FouladMetrics::values;
  const int sidePad = m.contentSidePadding;

  if (recentBooks.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, rect.y + rect.height / 3, tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height / 3 + 40, tr(STR_START_READING));
    return;
  }

  const int heroX = sidePad;
  const int heroY = rect.y;
  const int heroH = m.homeCoverHeight;
  const int textX = heroX + kHeroCoverWidth + kHeroGap;
  const int textW = rect.width - textX - sidePad;

  // Recents row geometry (below the hero card + section header).
  const int headerY = heroY + heroH + 22;
  const int rowY = headerY + 34;
  const int shownRecents = std::min(static_cast<int>(recentBooks.size()) - 1, 3);
  const int tileW = (rect.width - sidePad * 2 - kRecentsGap * 2) / 3;
  const int coverH2 = std::min(200, rect.y + rect.height - rowY - 30);

  // Static content (covers) is drawn once and captured into the cover buffer;
  // HomeActivity restores it on later renders so only text/selection redraw.
  if (!coverRendered) {
    drawCoverAt(renderer, recentBooks[0].coverBmpPath, heroX, heroY, kHeroCoverWidth, heroH);
    for (int i = 0; i < shownRecents; i++) {
      const int x = sidePad + i * (tileW + kRecentsGap);
      drawCoverAt(renderer, recentBooks[i + 1].coverBmpPath, x, rowY, tileW, coverH2);
    }
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  // --- Hero text column (redrawn every render) ---
  const BookReadingStats heroStats = BookReadingStats::load(readingStatsCachePathForBook(recentBooks[0].path));
  int y = heroY + 2;
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, recentBooks[0].title.c_str(), textW, 3);
  for (const auto& line : titleLines) {
    renderer.drawTextInWidth(UI_12_FONT_ID, textX, y, textW, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 2;
  }
  if (!recentBooks[0].author.empty()) {
    const auto author = renderer.truncatedText(UI_10_FONT_ID, recentBooks[0].author.c_str(), textW);
    renderer.drawTextInWidth(UI_10_FONT_ID, textX, y + 4, textW, author.c_str());
  }
  y += renderer.getLineHeight(UI_10_FONT_ID) + 18;

  // Progress bar + percentage from the cached reader-exit progress.
  {
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%u%%", static_cast<unsigned>(heroStats.lastProgressPercent));
    const int pctW = renderer.getTextWidth(UI_10_FONT_ID, pctBuf, EpdFontFamily::BOLD);
    const int barW = textW - pctW - 10;
    const int barH = 12;
    renderer.drawRect(textX, y, barW, barH, true);
    const int fillW = (barW - 4) * heroStats.lastProgressPercent / 100;
    if (fillW > 0) renderer.fillRect(textX + 2, y + 2, fillW, barH - 4);
    renderer.drawText(UI_10_FONT_ID, textX + barW + 10, y + barH / 2 - renderer.getLineHeight(UI_10_FONT_ID) / 2,
                      pctBuf, true, EpdFontFamily::BOLD);
    y += barH + 14;
  }

  // Read / Est. Left readouts (two columns).
  {
    char readBuf[24];
    char leftBuf[24];
    formatReadingDuration(heroStats.totalReadingSeconds, readBuf, sizeof(readBuf));
    if (heroStats.estimatedTimeLeftSeconds > 0) {
      leftBuf[0] = '~';
      formatReadingDuration(heroStats.estimatedTimeLeftSeconds, leftBuf + 1, sizeof(leftBuf) - 1);
    } else {
      snprintf(leftBuf, sizeof(leftBuf), "-");
    }
    const int colW = textW / 2;
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    renderer.drawText(UI_10_FONT_ID, textX, y, tr(STR_READ_LABEL), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, textX + colW, y, tr(STR_EST_LEFT), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, textX, y + lineH + 4, readBuf);
    renderer.drawText(UI_10_FONT_ID, textX + colW, y + lineH + 4, leftBuf);
  }

  // --- Recents section header ---
  {
    const char* label = tr(STR_RECENTS);
    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
    const int labelX = (rect.width - labelW) / 2;
    const int lineY = headerY + renderer.getLineHeight(UI_10_FONT_ID) / 2;
    renderer.drawLine(sidePad, lineY, labelX - 10, lineY, true);
    renderer.drawLine(labelX + labelW + 10, lineY, rect.width - sidePad, lineY, true);
    renderer.drawText(UI_10_FONT_ID, labelX, headerY, label, true, EpdFontFamily::BOLD);
  }

  // --- Recents row dynamic parts: titles, progress bars, +N badge ---
  for (int i = 0; i < shownRecents; i++) {
    const int x = sidePad + i * (tileW + kRecentsGap);
    const auto& book = recentBooks[i + 1];
    const BookReadingStats stats = BookReadingStats::load(readingStatsCachePathForBook(book.path));

    // Mini progress bar hugging the cover's bottom edge.
    if (stats.lastProgressPercent > 0) {
      const int fillW = (tileW - 8) * stats.lastProgressPercent / 100;
      renderer.fillRect(x + 4, rowY + coverH2 - 8, tileW - 8, 4, false);
      renderer.drawRect(x + 4, rowY + coverH2 - 8, tileW - 8, 4, true);
      if (fillW > 0) renderer.fillRect(x + 4, rowY + coverH2 - 8, fillW, 4, true);
    }

    const auto title = renderer.truncatedText(SMALL_FONT_ID, book.title.c_str(), tileW);
    const int titleW = renderer.getTextWidth(SMALL_FONT_ID, title.c_str());
    renderer.drawText(SMALL_FONT_ID, x + std::max(0, (tileW - titleW) / 2), rowY + coverH2 + 6, title.c_str());
  }

  // Count badge on the last shown recents cover when more books exist beyond the
  // row (the screenshot's "16" badge): total recents in the store minus the hero
  // and the covers already visible.
  const int totalRecents = static_cast<int>(RECENT_BOOKS.getBooks().size());
  const int hiddenCount = totalRecents - 1 - shownRecents;
  if (shownRecents == 3 && hiddenCount > 0) {
    char badge[8];
    snprintf(badge, sizeof(badge), "%d", std::min(hiddenCount, 99));
    const int bx = sidePad + 2 * (tileW + kRecentsGap) + tileW - 16;
    const int by = rowY + 16;
    renderer.fillRoundedRect(bx - 15, by - 15, 30, 30, 15, Color::Black);
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, badge, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, bx - tw / 2, by - renderer.getLineHeight(SMALL_FONT_ID) / 2, badge, false,
                      EpdFontFamily::BOLD);
  }

  // --- Selection frames ---
  if (selectorIndex == 0) {
    renderer.drawRect(heroX - kSelectionBorder - 1, heroY - kSelectionBorder - 1,
                      kHeroCoverWidth + 2 * (kSelectionBorder + 1), heroH + 2 * (kSelectionBorder + 1),
                      kSelectionBorder, true);
  } else if (selectorIndex >= 1 && selectorIndex <= shownRecents) {
    const int i = selectorIndex - 1;
    const int x = sidePad + i * (tileW + kRecentsGap);
    renderer.drawRect(x - kSelectionBorder - 1, rowY - kSelectionBorder - 1, tileW + 2 * (kSelectionBorder + 1),
                      coverH2 + 2 * (kSelectionBorder + 1), kSelectionBorder, true);
  }
}

void FouladTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, const int buttonCount, const int selectedIndex,
                                 const std::function<std::string(int index)>& buttonLabel,
                                 const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) return;
  constexpr int kIconSize = 32;
  constexpr int kLabelGap = 4;
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int barH = kIconSize + kLabelGap + labelLineH + 16;
  const int barY = rect.y + std::max(0, rect.height - barH);
  const int slotW = rect.width / buttonCount;
  // RTL UI languages flow the bar right-to-left (first item at the rightmost slot).
  const bool rtl = I18N.isRtl();

  renderer.drawLine(rect.x, barY - 2, rect.x + rect.width - 1, barY - 2, true);

  for (int i = 0; i < buttonCount; i++) {
    const int slotIndex = rtl ? buttonCount - 1 - i : i;
    const int slotX = rect.x + slotIndex * slotW;
    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.drawRoundedRect(slotX + 3, barY + 2, slotW - 6, barH - 4, /*lineWidth=*/2, /*cornerRadius=*/8, true);
    }

    const uint8_t* icon = rowIcon != nullptr ? menuIconBitmap(rowIcon(i)) : nullptr;
    const int iconY = barY + 8;
    if (icon != nullptr) {
      renderer.drawIcon(icon, slotX + (slotW - kIconSize) / 2, iconY, kIconSize);
    }

    const std::string label = renderer.truncatedText(SMALL_FONT_ID, buttonLabel(i).c_str(), slotW - 10,
                                                     selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int labelW =
        renderer.getTextWidth(SMALL_FONT_ID, label.c_str(), selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, slotX + std::max(0, (slotW - labelW) / 2), iconY + kIconSize + kLabelGap,
                      label.c_str(), true, selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}
