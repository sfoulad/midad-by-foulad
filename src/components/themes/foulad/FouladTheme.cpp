#include "FouladTheme.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <ScriptDetector.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/apps.h"
#include "components/icons/bluetooth.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/stats.h"
#include "components/icons/transfer.h"
#include "fontIds.h"
#include "reading/ReadingStatsStore.h"
#ifndef SIMULATOR
// No simulator-side counterpart exists for this HAL component -- see main.cpp's own
// guarded include of the same header. bleActive below stays false in simulator
// builds, so this whole indicator compiles out to nothing rather than needing a
// simulator-side stub.
#include <BlePeripheralManager.h>
#endif

// Home layout ported from aalu (github.com/dawsonfi/aalu) HomeRenderer: top status
// line, 200x300 hero cover with a metadata column (title / author / rounded pill
// progress bar + % / Read + Est. Left columns), a double-rule "Recents" divider,
// a 140x210 three-cover thumbnail row with floating progress pills and a stacked
// +N badge, and a rule-framed bottom icon menu with a rounded-outline selection.
// Data sources are ours (RecentBooksStore + ReadingStatsStore), only the visual
// language comes from aalu.
namespace {
constexpr int kStatusBarHeight = 30;
constexpr int kHeroPadding = 20;
constexpr int kHeroCoverWidth = 200;
constexpr int kHeroHeight = 300;
constexpr int kHeroMetaGap = 20;
constexpr int kCoverCornerRadius = 6;

// Thumb width is derived from the rect width at draw time so the row fills the
// screen on both devices (X4 portrait 480 -> 136px tiles, X3 portrait 528 ->
// 152px tiles) instead of leaving dead margins on the wider X3 panel.
constexpr int kThumbCoverHeight = 210;
constexpr int kThumbGap = 20;
constexpr int kThumbsCount = 3;
constexpr int kDividerHalfSeparation = 18;

constexpr int kMenuPadding = 16;
constexpr int kMenuGap = 8;
constexpr int kMenuBandHeight = 72;
constexpr int kMenuIconSize = 32;
constexpr int kMenuIconLabelGap = 6;

constexpr int kCompletedPercent = 95;

const uint8_t* menuIconBitmap(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Folder:
      // Home slot 0 shows Files (SD browser) when no Foulad eBooks account
      // is configured -- see HomeActivity's fouladEbooksLoggedIn().
      return FolderIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Stats:
      return StatsIcon;
    case UIIcon::Apps:
      return AppsIcon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    default:
      return nullptr;
  }
}

// Brute-force filled disc, like aalu's badge helper: cheap at badge radii and
// avoids a new GfxRenderer primitive.
void fillDisc(const GfxRenderer& renderer, const int cx, const int cy, const int radius, const bool black) {
  const int rsq = radius * radius;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      if (dx * dx + dy * dy <= rsq) {
        renderer.drawPixel(cx + dx, cy + dy, black);
      }
    }
  }
}

// aalu's rounded "pill" progress bar: 1px rounded outline, fill rounded only on
// the leading corners so the trailing edge reads as a straight cut.
void drawRoundedProgressBar(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                            const int percent, const int maxRadius = 3, const bool rtlFill = false) {
  if (width <= 0 || height <= 0) return;
  const int radius = std::min({maxRadius, width / 2, height / 2});
  renderer.drawRoundedRect(x, y, width, height, 1, radius, true);
  const int clamped = std::clamp(percent, 0, 100);
  const int innerW = width - 2;
  const int innerH = height - 2;
  if (innerW <= 0 || innerH <= 0) return;
  const int fillW = innerW * clamped / 100;
  if (fillW <= 0) return;
  const int fillRadius = std::max(0, radius - 1);
  if (fillW >= innerW) {
    renderer.fillRoundedRect(x + 1, y + 1, innerW, innerH, fillRadius, Color::Black);
  } else if (rtlFill) {
    // RTL: progress advances right-to-left; the leading (right) corners round.
    renderer.fillRoundedRect(x + 1 + innerW - fillW, y + 1, fillW, innerH, fillRadius, /*roundTopLeft=*/false,
                             /*roundTopRight=*/true, /*roundBottomLeft=*/false, /*roundBottomRight=*/true,
                             Color::Black);
  } else {
    renderer.fillRoundedRect(x + 1, y + 1, fillW, innerH, fillRadius, /*roundTopLeft=*/true, /*roundTopRight=*/false,
                             /*roundBottomLeft=*/true, /*roundBottomRight=*/false, Color::Black);
  }
}

// Floating progress pill inside the cover bottom, ringed by a white halo so it
// stays legible on dark cover art (aalu drawCoverProgressBar).
void drawCoverProgressOverlay(const GfxRenderer& renderer, const int coverX, const int coverY, const int coverW,
                              const int coverH, const int percent, const bool rtl = false) {
  if (percent <= 0) return;
  if (percent >= kCompletedPercent) {
    // Completed: black disc + white check.
    const int radius = 11;
    const int cx = rtl ? coverX + radius + 4 : coverX + coverW - radius - 4;
    const int cy = coverY + radius + 4;
    fillDisc(renderer, cx, cy, radius, true);
    renderer.drawLine(cx - 5, cy, cx - 1, cy + 4, 2, false);
    renderer.drawLine(cx - 1, cy + 4, cx + 5, cy - 4, 2, false);
    return;
  }
  constexpr int kBarH = 6;
  constexpr int kMargin = 8;
  constexpr int kHalo = 2;
  const int barW = coverW - 2 * kMargin;
  if (barW <= 0 || coverH <= kBarH + kMargin) return;
  const int barX = coverX + kMargin;
  const int barY = coverY + coverH - kBarH - kMargin;
  renderer.fillRoundedRect(barX - kHalo, barY - kHalo, barW + 2 * kHalo, kBarH + 2 * kHalo, 2 + kHalo, Color::White);
  drawRoundedProgressBar(renderer, barX, barY, barW, kBarH, percent, 2, rtl);
}

// Ghost "pages" behind a stacked cover (aalu drawBackStack): offset white plates
// with 1px rounded outlines, up-right of the cover.
void drawBackStack(const GfxRenderer& renderer, const int x, const int y, const int width, const int height, int depth,
                   const bool rtl = false) {
  if (depth <= 0) return;
  if (depth > 2) depth = 2;
  for (int g = depth; g >= 1; --g) {
    const int gx = x + (rtl ? -4 * g : 4 * g);
    const int gy = y - 4 * g;
    renderer.fillRoundedRect(gx, gy, width, height, kCoverCornerRadius, Color::White);
    renderer.drawRoundedRect(gx, gy, width, height, 1, kCoverCornerRadius, true);
  }
}

// Round count badge at a cover's top-right corner (aalu drawRoundCountBadge).
void drawRoundCountBadge(const GfxRenderer& renderer, const int coverX, const int coverY, const int coverW,
                         const char* count, const bool rtl = false) {
  const int radius = 13;
  const int cx = rtl ? coverX + radius + 3 : coverX + coverW - radius - 3;
  const int cy = coverY + radius + 3;
  fillDisc(renderer, cx, cy, radius + 2, false);  // white halo ring
  fillDisc(renderer, cx, cy, radius, true);
  const int tw = renderer.getTextWidth(SMALL_FONT_ID, count, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, cx - tw / 2, cy - renderer.getLineHeight(SMALL_FONT_ID) / 2, count, false,
                    EpdFontFamily::BOLD);
}

// Cover paint. thumbHeight selects which generated thumbnail file to load --
// HomeActivity::loadRecentCovers only generates thumbs at homeCoverHeight (the
// hero size), so EVERY tile loads that one size and scales it into its own box
// (drawBitmap only ever scales down). Requesting the tile's own height here was
// why the recents-row covers showed placeholders: thumb_210.bmp never exists.
void drawCoverAt(const GfxRenderer& renderer, const std::string& coverBmpPath, const int x, const int y, const int w,
                 const int h, const int thumbHeight) {
  bool hasCover = false;
  if (!coverBmpPath.empty()) {
    const std::string path = UITheme::getCoverThumbPath(coverBmpPath, thumbHeight);
    HalFile file;
    if (Storage.openFileForRead("HOME", path, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        if (bitmap.getHeight() == h) {
          // Exact-height thumb (the hero): crop horizontally to fill the box.
          const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float tileRatio = static_cast<float>(w) / static_cast<float>(h);
          const float cropX = std::max(0.0f, 1.0f - (tileRatio / ratio));
          renderer.drawBitmap(bitmap, x, y, w, h, cropX);
        } else {
          // Larger thumb reused for a smaller tile: shrink-to-fit, centered.
          const float scale =
              std::min(static_cast<float>(w) / bitmap.getWidth(), static_cast<float>(h) / bitmap.getHeight());
          const int scaledW = static_cast<int>(bitmap.getWidth() * scale);
          const int scaledH = static_cast<int>(bitmap.getHeight() * scale);
          renderer.drawBitmap(bitmap, x + std::max(0, (w - scaledW) / 2), y + std::max(0, (h - scaledH) / 2), w, h);
        }
        hasCover = true;
      }
    }
  }
  renderer.drawRoundedRect(x, y, w, h, 1, kCoverCornerRadius, true);
  if (!hasCover) {
    renderer.fillRect(x + 1, y + h / 3, w - 2, 2 * h / 3 - 1, true);
    renderer.drawIcon(CoverIcon, x + (w - 32) / 2, y + h / 3 + h / 6 - 16, 32);
  }
}

// Concentric triple rounded outlines around a focused cover (aalu
// drawSelectionBorder, cover variant).
void drawCoverSelection(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  renderer.drawRoundedRect(x - 2, y - 2, w + 4, h + 4, 1, kCoverCornerRadius + 2, true);
  renderer.drawRoundedRect(x - 3, y - 3, w + 6, h + 6, 1, kCoverCornerRadius + 3, true);
  renderer.drawRoundedRect(x - 4, y - 4, w + 8, h + 8, 1, kCoverCornerRadius + 4, true);
}
}  // namespace

void FouladTheme::drawBleStatusIcon(const GfxRenderer& renderer, const Rect batteryRect, const bool rtl,
                                    const bool showBatteryPercentage) const {
  bool bleActive = false;
#ifndef SIMULATOR
  bleActive = BlePeripheral.isActive();
#endif
  if (!bleActive) return;

  // BLE-R2 semantics: the global indicator shows only for Advertising/Connected
  // (isActive() covers exactly those two) -- Off and PausedLowMemory show nothing
  // here, since BluetoothActivity is where those states get explained in detail.
  constexpr int kIconSize = 20;
  int batteryTextWidth = 0;
  if (showBatteryPercentage) {
    const uint16_t percentage = powerManager.getBatteryPercentage();
    batteryTextWidth =
        batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, (std::to_string(percentage) + "%").c_str());
  }
  // Mirrors drawBatteryLeft/Right's own internal "+6" vertical offset (BaseTheme.cpp)
  // and centers the icon on the battery outline's actual vertical center, not the
  // Rect's nominal y -- see this method's own header-comment for why a fixed guessed
  // offset is exactly the bug BLE-R2's audit found and avoided.
  const int iconY = batteryRect.y + 6 + batteryRect.height / 2 - kIconSize / 2;
  if (rtl) {
    renderer.drawIcon(BluetoothIcon, batteryRect.x + batteryRect.width + 4 + batteryTextWidth, iconY, kIconSize);
  } else {
    renderer.drawIcon(BluetoothIcon, batteryRect.x - kIconSize - 4 - batteryTextWidth, iconY, kIconSize);
  }
}

void FouladTheme::drawHeader(const GfxRenderer& renderer, const Rect rect, const char* title,
                             const char* subtitle) const {
  // Delegate the entire header to LyraTheme -- every screen except Home routes
  // through this, and duplicating its title/subtitle truncation and layout logic
  // here would be real, ongoing upstream-conflict surface for no benefit. Only the
  // BLE overlay below is genuinely Midad-specific.
  LyraTheme::drawHeader(renderer, rect, title, subtitle);

  const bool rtl = I18N.isRtl();
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Same battery rect LyraTheme::drawHeader() itself just used internally -- not
  // exposed by that call, so recomputed here from the same rect/metrics it started
  // from. Small, self-contained arithmetic; see FouladTheme.h's own comment on why
  // this trades a few duplicated lines for not touching the upstream-shared file.
  const Rect batteryRect =
      rtl ? Rect{rect.x + 12, rect.y + 5, LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight}
          : Rect{rect.x + rect.width - 12 - LyraMetrics::values.batteryWidth, rect.y + 5,
                 LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight};
  drawBleStatusIcon(renderer, batteryRect, rtl, showBatteryPercentage);
}

void FouladTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                      const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                      bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  // Full RTL mirroring under the Arabic UI: hero cover moves to the right with
  // the metadata column on the left (text right-aligned), the thumbnail slots
  // and the Read/Est.Left columns flow right-to-left, badges/stacks mirror to
  // the top-left corner, and the status line swaps clock/battery sides.
  const bool rtl = I18N.isRtl();
  const int heroCoverX = rtl ? rect.x + rect.width - kHeroPadding - kHeroCoverWidth : rect.x + kHeroPadding;
  const int heroCoverY = rect.y + kStatusBarHeight;
  const int metaX = rtl ? rect.x + kHeroPadding : heroCoverX + kHeroCoverWidth + kHeroMetaGap;
  const int metaWidth = rect.width - kHeroCoverWidth - kHeroMetaGap - kHeroPadding * 2;

  // Recents band geometry: divider label centered between two rules, thumbs below.
  const int dividerLabelY = heroCoverY + kHeroHeight + kDividerHalfSeparation + 10;
  // Arabic UI text draws through whatever font UI_12_FONT_ID's Arabic dispatch
  // actually resolves to (Tajawal -- see ArabicFontSystem::applyArabicMappings),
  // which sits taller/deeper than the Latin UI_12 (Inter) metrics this block was
  // originally tuned against. Resolve and measure the font that will REALLY draw
  // the label -- not a hardcoded guess at which font that is -- so the divider
  // rule gap and the thumbnail row start can never drift out of sync with what's
  // actually on screen again, no matter which font ends up serving Arabic UI_12
  // text in the future. dividerLabelH/dividerSep/dividerBottomRuleY are reused
  // verbatim by the "Double-rule Recents divider" block below instead of being
  // recomputed there, so the two can't drift apart a second time either.
  const char* dividerLabelText = tr(STR_RECENTS);
  const bool arabicDividerLabel = ScriptDetector::containsArabic(dividerLabelText);
  const int latinUi12LineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int dividerLabelH = arabicDividerLabel ? renderer.getLineHeight(renderer.getResolvedArabicFontId(UI_12_FONT_ID))
                                               : latinUi12LineHeight;
  const int dividerSep = kDividerHalfSeparation + (arabicDividerLabel ? 5 : 0);
  const int dividerBottomRuleY = dividerLabelY + dividerLabelH / 2 + dividerSep;
  // Gap tuned against the original Latin-only formula (dividerLabelY +
  // kDividerHalfSeparation + 28, which never referenced label height at all --
  // 28px clears the bottom rule with a visible gap, 16 left covers touching it)
  // re-expressed as an offset from the bottom rule itself, so the exact same
  // formula now stays correct for any label font/height, Latin or Arabic.
  const int thumbsY = dividerBottomRuleY + (28 - latinUi12LineHeight / 2);
  const int shownRecents = std::min(static_cast<int>(recentBooks.size()) - 1, kThumbsCount);
  const int kThumbWidth = (rect.width - 2 * kMenuPadding - (kThumbsCount - 1) * kThumbGap) / kThumbsCount;
  const int thumbsTotalW = kThumbsCount * kThumbWidth + (kThumbsCount - 1) * kThumbGap;
  const int thumbsX = rect.x + (rect.width - thumbsTotalW) / 2;

  // --- Static covers, drawn once and captured into the stored frame buffer ---
  if (!coverRendered && !recentBooks.empty()) {
    drawCoverAt(renderer, recentBooks[0].coverBmpPath, heroCoverX, heroCoverY, kHeroCoverWidth, kHeroHeight,
                kHeroHeight);
    const int totalRecents = static_cast<int>(RECENT_BOOKS.getBooks().size());
    for (int i = 0; i < shownRecents; i++) {
      const int slot = rtl ? kThumbsCount - 1 - i : i;
      const int x = thumbsX + slot * (kThumbWidth + kThumbGap);
      const bool isStackTile = (i == kThumbsCount - 1) && totalRecents - 1 > kThumbsCount;
      if (isStackTile) {
        drawBackStack(renderer, x, thumbsY, kThumbWidth, kThumbCoverHeight,
                      std::min(2, totalRecents - 1 - kThumbsCount), rtl);
      }
      drawCoverAt(renderer, recentBooks[i + 1].coverBmpPath, x, thumbsY, kThumbWidth, kThumbCoverHeight, kHeroHeight);
    }
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  // --- Status line: clock left, app name centered, battery drawn by drawHeader
  // pattern (right). Redrawn every render so the clock stays current. ---
  {
    const bool showBatteryPct =
        SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
    if (halClock.isAvailable()) {
      char timeBuf[9];
      if (halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
        const int clockX = rtl ? rect.x + rect.width - kHeroPadding - renderer.getTextWidth(SMALL_FONT_ID, timeBuf)
                               : rect.x + kHeroPadding;
        renderer.drawText(SMALL_FONT_ID, clockX, rect.y, timeBuf);
      }
    }
    renderer.drawCenteredText(SMALL_FONT_ID, rect.y, tr(STR_BRAND_MIDAD), true, EpdFontFamily::BOLD);
    // Home draws this status line itself rather than calling drawHeader() (see this
    // block's own leading comment), so it never gets the BLE indicator by
    // inheritance the way every other screen does via FouladTheme::drawHeader()
    // above -- added explicitly here, same helper, same reasoning.
    if (rtl) {
      const Rect batteryRect{rect.x + kHeroPadding, rect.y - 5, FouladMetrics::values.batteryWidth,
                             FouladMetrics::values.batteryHeight};
      drawBatteryLeft(renderer, batteryRect, showBatteryPct);
      drawBleStatusIcon(renderer, batteryRect, rtl, showBatteryPct);
    } else {
      const int batteryX = rect.x + rect.width - kHeroPadding - FouladMetrics::values.batteryWidth;
      const Rect batteryRect{batteryX, rect.y - 5, FouladMetrics::values.batteryWidth,
                             FouladMetrics::values.batteryHeight};
      drawBatteryRight(renderer, batteryRect, showBatteryPct);
      drawBleStatusIcon(renderer, batteryRect, rtl, showBatteryPct);
    }
  }

  if (recentBooks.empty()) {
    renderer.drawRect(heroCoverX, heroCoverY, rect.width - kHeroPadding * 2, kHeroHeight, true);
    renderer.drawCenteredText(UI_12_FONT_ID, heroCoverY + kHeroHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID),
                              tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, heroCoverY + kHeroHeight / 2 + 8, tr(STR_START_READING));
    return;
  }

  // --- Hero metadata column (title -> author -> progress pill + % -> Read/Left) ---
  READING_STATS.ensureLoaded();
  const ReadingBookStats* heroBook = READING_STATS.findBook(recentBooks[0].path);
  const uint8_t heroProgressPercent = heroBook ? heroBook->lastProgressPercent : 0;
  const uint32_t heroReadSeconds = heroBook ? static_cast<uint32_t>(heroBook->totalReadingMs / 1000ULL) : 0;
  const uint32_t heroEstLeftSeconds = heroBook ? heroBook->estimatedTimeLeftSeconds : 0;
  {
    const int heroBottom = heroCoverY + kHeroHeight - 4;
    const bool titleArabic = ScriptDetector::containsArabic(recentBooks[0].title.c_str());
    // LEXENDDECA_18_FONT_ID doubles as the real EPUB body-text font at XLarge
    // Latin size when the user's reading font is Bitter (see
    // CrossPointSettings::getReaderFontId), so mapping IT to Tajawal globally
    // (in ArabicFontSystem::applyArabicMappings) would hijack actual book
    // reading text away from the user's chosen Arabic reading font (e.g.
    // UthmanicHafs for the Quran) whenever they read at that size. This hero
    // title is UI chrome, not reading body text, so it forces Tajawal
    // directly and locally instead -- same "Tajawal for all framework Arabic
    // UI" rule as the author line below (already Tajawal via UI_10_FONT_ID's
    // mapping) and the headers/lists/grid titles elsewhere. The Latin hero
    // title itself is deliberately always Bitter regardless of the user's
    // chosen reading font (Bitter/Lexend Deca) -- fixed chrome styling, not
    // meant to track that setting.
    const int titleFontId = titleArabic ? TAJAWAL_18_FONT_ID : LEXENDDECA_18_FONT_ID;
    const int titleLineHeight = renderer.getLineHeight(titleFontId);
    int textY = heroCoverY + 6;
    const auto titleLines =
        renderer.wrappedText(titleFontId, recentBooks[0].title.c_str(), metaWidth, /*maxLines=*/2, EpdFontFamily::BOLD);
    for (const auto& line : titleLines) {
      if (rtl) {
        const int w = renderer.getTextWidth(titleFontId, line.c_str(), EpdFontFamily::BOLD);
        renderer.drawText(titleFontId, metaX + metaWidth - w, textY, line.c_str(), true, EpdFontFamily::BOLD);
      } else {
        renderer.drawTextInWidth(titleFontId, metaX, textY, metaWidth, line.c_str(), true, EpdFontFamily::BOLD);
      }
      if (titleArabic) {
        // No bold Arabic face ships in the firmware; double-strike 1px apart
        // reads as bold on the 1-bit panel, matching the Latin BOLD title.
        if (rtl) {
          const int w = renderer.getTextWidth(titleFontId, line.c_str(), EpdFontFamily::BOLD);
          renderer.drawText(titleFontId, metaX + metaWidth - w + 1, textY, line.c_str(), true, EpdFontFamily::BOLD);
        } else {
          renderer.drawTextInWidth(titleFontId, metaX + 1, textY, metaWidth, line.c_str(), true, EpdFontFamily::BOLD);
        }
      }
      textY += titleLineHeight + 2;
    }
    textY += 4;

    if (!recentBooks[0].author.empty()) {
      const std::string author = renderer.truncatedText(UI_10_FONT_ID, recentBooks[0].author.c_str(), metaWidth);
      if (rtl) {
        renderer.drawText(UI_10_FONT_ID, metaX + metaWidth - renderer.getTextWidth(UI_10_FONT_ID, author.c_str()),
                          textY, author.c_str());
      } else {
        renderer.drawTextInWidth(UI_10_FONT_ID, metaX, textY, metaWidth, author.c_str());
      }
      textY += renderer.getLineHeight(UI_10_FONT_ID) + 12;
    } else {
      textY += 8;
    }

    // Rounded pill progress bar + bold percent label.
    constexpr int kBarHeight = 10;
    constexpr int kLabelGapPx = 8;
    const int pct = std::clamp(static_cast<int>(heroProgressPercent), 0, 100);
    char percentStr[8];
    snprintf(percentStr, sizeof(percentStr), "%d%%", pct);
    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, percentStr, EpdFontFamily::BOLD);
    const int barW = std::max(0, metaWidth - labelW - kLabelGapPx);
    if (textY + kBarHeight <= heroBottom && barW > 0) {
      const int barX = rtl ? metaX + labelW + kLabelGapPx : metaX;
      const int pctX = rtl ? metaX : metaX + barW + kLabelGapPx;
      drawRoundedProgressBar(renderer, barX, textY, barW, kBarHeight, pct, 3, rtl);
      renderer.drawText(UI_10_FONT_ID, pctX, textY + (kBarHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2 - 1,
                        percentStr, true, EpdFontFamily::BOLD);
      textY += kBarHeight + 12;
    }

    // Read / Est. Left, two columns: bold label above, value below.
    char readBuf[24];
    char leftBuf[24];
    formatReadingDuration(heroReadSeconds, readBuf, sizeof(readBuf));
    if (heroEstLeftSeconds > 0 && pct < 100) {
      leftBuf[0] = '~';
      formatReadingDuration(heroEstLeftSeconds, leftBuf + 1, sizeof(leftBuf) - 1);
    } else {
      snprintf(leftBuf, sizeof(leftBuf), "-");
    }
    const int labelLineH = renderer.getLineHeight(UI_10_FONT_ID);
    if (textY + labelLineH * 2 + 2 <= heroBottom && heroReadSeconds > 0) {
      const int colWidth = metaWidth / 2;
      const auto colText = [&](const int col, const int y, const char* text, const bool bold) {
        // Column 0 = "Read": right column in RTL, left column otherwise. Text
        // right-aligns inside its column under RTL.
        const int colX = metaX + ((col == 0) != rtl ? 0 : colWidth);
        const int x =
            rtl ? colX + colWidth - 8 -
                      renderer.getTextWidth(UI_10_FONT_ID, text, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR)
                : colX;
        renderer.drawText(UI_10_FONT_ID, x, y, text, true, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      };
      colText(0, textY, tr(STR_READ_LABEL), true);
      colText(1, textY, tr(STR_EST_LEFT), true);
      colText(0, textY + labelLineH + 2, readBuf, false);
      colText(1, textY + labelLineH + 2, leftBuf, false);
    }
  }

  // --- Double-rule "Recents" divider ---
  {
    const int x1 = rect.x + kMenuPadding;
    const int x2 = rect.x + rect.width - kMenuPadding;
    // dividerLabelH/dividerSep/dividerBottomRuleY computed once above (shared
    // with thumbsY) so this block's rule lines and the thumbnail row start can
    // never drift apart again.
    const int labelCenterY = dividerLabelY + dividerLabelH / 2;
    renderer.drawLine(x1, labelCenterY - dividerSep, x2, labelCenterY - dividerSep, true);
    renderer.drawLine(x1, dividerBottomRuleY, x2, dividerBottomRuleY, true);
    const int labelW = renderer.getTextWidth(UI_12_FONT_ID, dividerLabelText, EpdFontFamily::BOLD);
    const int labelX = (renderer.getScreenWidth() - labelW) / 2;
    renderer.drawText(UI_12_FONT_ID, labelX, dividerLabelY, dividerLabelText, true, EpdFontFamily::BOLD);
    if (arabicDividerLabel) {
      // No bold Arabic face ships in the firmware; double-strike 1px apart
      // reads as bold on the 1-bit panel, matching the Latin BOLD label.
      renderer.drawText(UI_12_FONT_ID, labelX + 1, dividerLabelY, dividerLabelText, true, EpdFontFamily::BOLD);
    }
  }

  // --- Thumbnail row dynamic parts: progress overlays and count badge. No title
  // labels under the covers -- the cover art already carries the book name. ---
  const int totalRecents = static_cast<int>(RECENT_BOOKS.getBooks().size());
  for (int i = 0; i < shownRecents; i++) {
    const int slot = rtl ? kThumbsCount - 1 - i : i;
    const int x = thumbsX + slot * (kThumbWidth + kThumbGap);
    const auto& book = recentBooks[i + 1];
    const bool isStackTile = (i == kThumbsCount - 1) && totalRecents - 1 > kThumbsCount;

    if (isStackTile) {
      char countBuf[8];
      snprintf(countBuf, sizeof(countBuf), "%d", std::min(totalRecents - 1 - kThumbsCount + 1, 99));
      drawRoundCountBadge(renderer, x, thumbsY, kThumbWidth, countBuf, rtl);
    } else {
      const ReadingBookStats* stats = READING_STATS.findBook(book.path);
      drawCoverProgressOverlay(renderer, x, thumbsY, kThumbWidth, kThumbCoverHeight,
                               stats ? stats->lastProgressPercent : 0, rtl);
    }
  }

  // --- Selection: triple concentric rounded outline around the focused cover ---
  if (selectorIndex == 0) {
    drawCoverSelection(renderer, heroCoverX, heroCoverY, kHeroCoverWidth, kHeroHeight);
  } else if (selectorIndex >= 1 && selectorIndex <= shownRecents) {
    const int slot = rtl ? kThumbsCount - 1 - (selectorIndex - 1) : selectorIndex - 1;
    const int x = thumbsX + slot * (kThumbWidth + kThumbGap);
    drawCoverSelection(renderer, x, thumbsY, kThumbWidth, kThumbCoverHeight);
  }
}

void FouladTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, const int buttonCount, const int selectedIndex,
                                 const std::function<std::string(int index)>& buttonLabel,
                                 const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) return;
  // Bottom-anchor the band at the screen edge (the home screen draws no
  // button-hints bar), regardless of the (looser) rect HomeActivity hands us.
  // The strip below the band holds the physical-button glyphs (see below).
  constexpr int kGlyphStripHeight = 20;
  const int barY = renderer.getScreenHeight() - kMenuBandHeight - kGlyphStripHeight;
  const int lineX1 = rect.x + kMenuPadding;
  const int lineX2 = rect.x + rect.width - kMenuPadding;
  renderer.drawLine(lineX1, barY, lineX2, barY, true);
  renderer.drawLine(lineX1, barY + kMenuBandHeight - 1, lineX2, barY + kMenuBandHeight - 1, true);

  const int totalGaps = kMenuGap * (buttonCount - 1);
  const int tileWidth = (rect.width - kMenuPadding * 2 - totalGaps) / buttonCount;
  // RTL UI languages flow the bar right-to-left (first item at the rightmost slot).
  const bool rtl = I18N.isRtl();

  for (int i = 0; i < buttonCount; i++) {
    const int slotIndex = rtl ? buttonCount - 1 - i : i;
    const int tileX = rect.x + kMenuPadding + slotIndex * (tileWidth + kMenuGap);
    const bool selected = selectedIndex == i;
    const uint8_t* icon = rowIcon != nullptr ? menuIconBitmap(rowIcon(i)) : nullptr;

    if (selected) {
      // aalu selection: icon only, centered, framed by a double rounded outline.
      const int iconY = barY + (kMenuBandHeight - kMenuIconSize) / 2;
      if (icon != nullptr) {
        renderer.drawIcon(icon, tileX + (tileWidth - kMenuIconSize) / 2, iconY, kMenuIconSize);
      }
      renderer.drawRoundedRect(tileX + 2, barY + 2, tileWidth - 4, kMenuBandHeight - 4, 1, 6, true);
      renderer.drawRoundedRect(tileX + 3, barY + 3, tileWidth - 6, kMenuBandHeight - 6, 1, 5, true);
      continue;
    }

    const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
    const int contentH = kMenuIconSize + kMenuIconLabelGap + labelLineH;
    const int iconY = barY + (kMenuBandHeight - contentH) / 2;
    if (icon != nullptr) {
      renderer.drawIcon(icon, tileX + (tileWidth - kMenuIconSize) / 2, iconY, kMenuIconSize);
    }
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, buttonLabel(i).c_str(), tileWidth - 6);
    const int labelW = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
    renderer.drawText(SMALL_FONT_ID, tileX + std::max(0, (tileWidth - labelW) / 2),
                      iconY + kMenuIconSize + kMenuIconLabelGap, label.c_str());
  }

  // Physical-button glyphs in the strip under the band, one per tile, as a visual
  // hint of the front button below the bezel. The glyphs mark PHYSICAL bezel
  // positions, so unlike the tiles above they must NOT mirror in RTL -- the
  // buttons don't move when the UI language flips (user-specified Arabic
  // layout: nothing under Settings, dot under Update, left triangle under
  // Stats, right triangle under eBooks -- i.e. the same screen slots as LTR).
  // Drawn as vector shapes -- the bundled fonts have no U+25C0/25B6/25CF glyphs.
  enum class ButtonGlyph : uint8_t { None, Dot, TriangleLeft, TriangleRight };
  static constexpr ButtonGlyph kHomeMenuGlyphs[] = {ButtonGlyph::None, ButtonGlyph::Dot, ButtonGlyph::TriangleLeft,
                                                    ButtonGlyph::TriangleRight};
  constexpr int kGlyphCount = sizeof(kHomeMenuGlyphs) / sizeof(kHomeMenuGlyphs[0]);
  constexpr int kGlyphH = 10;
  constexpr int kGlyphW = 8;
  const int glyphCy = barY + kMenuBandHeight + 4 + kGlyphH / 2;
  for (int i = 0; i < buttonCount && i < kGlyphCount; i++) {
    const int tileX = rect.x + kMenuPadding + i * (tileWidth + kMenuGap);
    const int cx = tileX + tileWidth / 2;
    switch (kHomeMenuGlyphs[i]) {
      case ButtonGlyph::Dot:
        fillDisc(renderer, cx, glyphCy, kGlyphH / 2 - 1, true);
        break;
      case ButtonGlyph::TriangleLeft:
        // Filled left-pointing triangle, one horizontal line per row.
        for (int row = 0; row < kGlyphH; row++) {
          const int half = kGlyphH / 2;
          const int inset = kGlyphW * std::abs(row - half) / half;
          renderer.drawLine(cx - kGlyphW / 2 + inset, glyphCy - half + row, cx + kGlyphW / 2, glyphCy - half + row,
                            true);
        }
        break;
      case ButtonGlyph::TriangleRight:
        for (int row = 0; row < kGlyphH; row++) {
          const int half = kGlyphH / 2;
          const int inset = kGlyphW * std::abs(row - half) / half;
          renderer.drawLine(cx - kGlyphW / 2, glyphCy - half + row, cx + kGlyphW / 2 - inset, glyphCy - half + row,
                            true);
        }
        break;
      case ButtonGlyph::None:
        break;
    }
  }
}
