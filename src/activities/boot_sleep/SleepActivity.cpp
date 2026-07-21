#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <ScriptDetector.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "CuratedAyahs.h"
#include "QuranBook.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/stats/AppMetricCard.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"
#include "reading/ReadingStatsStore.h"

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool renderQuickResume =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);

  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  LOG_INF("SLP", "Sleep screen mode=%u fromReader=%d", SETTINGS.sleepScreen, (int)APP_STATE.lastSleepFromReader);
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    case (CrossPointSettings::SLEEP_SCREEN_MODE::DASHBOARD):
      return renderDashboardSleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over the /sleep folder.
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      if (dir) dir.close();
      return;
    }
    file.close();
  }

  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid BMP files
    for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
      if (dirFile.isDirectory()) {
        dirFile.close();
        continue;
      }
      dirFile.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        dirFile.close();
        continue;
      }

      if (!FsHelpers::hasBmpExtension(filename)) {
        LOG_DBG("SLP", "Skipping non-.bmp file name: %s", name);
        dirFile.close();
        continue;
      }
      Bitmap bitmap(dirFile);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
        dirFile.close();
        continue;
      }
      files.emplace_back(filename);
      dirFile.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Pick a random wallpaper, excluding recently shown ones.
      // Window: up to SLEEP_RECENT_COUNT entries, capped at numFiles-1.
      const uint16_t fileCount = static_cast<uint16_t>(std::min(numFiles, static_cast<size_t>(UINT16_MAX)));
      const uint8_t window =
          static_cast<uint8_t>(std::min(static_cast<size_t>(APP_STATE.recentSleepFill), numFiles - 1));
      auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
      for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
        randomFileIndex = static_cast<uint16_t>(random(fileCount));
      }
      APP_STATE.pushRecentSleep(randomFileIndex);
      APP_STATE.saveToFile();
      const auto filename = std::string(sleepDir) + "/" + files[randomFileIndex];
      HalFile randFile;
      if (Storage.openFileForRead("SLP", filename, randFile)) {
        LOG_DBG("SLP", "Randomly loading: %s/%s", sleepDir, files[randomFileIndex].c_str());
        delay(100);
        Bitmap bitmap(randFile, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          randFile.close();
          dir.close();
          return;
        }
        randFile.close();
      }
    }
  }
  if (dir) dir.close();

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  if (hasGreyscale) {
    // OEM grayscale pipeline base: use a full sleep-screen paint so the panel
    // enters deep sleep from a clean B/W baseline before the gray nudge refresh.
    renderer.displayGrayscaleBase(HalDisplay::FULL_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

// Cover + reading-stats screensaver, modeled on CrossInk's (uxjulia/crossink)
// "Dashboard" sleep theme but built from this fork's own components rather than
// a port: the cover/title/progress layout mirrors the Home screen's hero card
// (FouladTheme::drawRecentBookCover), and the stat figures reuse AppMetricCard +
// ReadingStatsStore exactly as the in-app Stats screen does (StatsActivity.cpp).
void SleepActivity::renderDashboardSleepScreen() const {
  if (APP_STATE.openEpubPath.empty()) {
    return renderDefaultSleepScreen();
  }

  const RecentBook book = RECENT_BOOKS.getDataFromBook(APP_STATE.openEpubPath);
  if (book.path.empty()) {
    return renderDefaultSleepScreen();
  }

  READING_STATS.ensureLoaded();
  const ReadingBookStats* bookStats = READING_STATS.findBook(book.path);
  const uint8_t progressPercent = bookStats ? bookStats->lastProgressPercent : 0;
  const uint32_t estimatedLeftSeconds = bookStats ? bookStats->estimatedTimeLeftSeconds : 0;

  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int sidePadding = UITheme::getInstance().getMetrics().contentSidePadding;

  // Rotating ayah quote, only when the Quran feature is actually enabled+extracted
  // (QuranBook::isPinned()) -- otherwise the layout is unchanged from before. A
  // full short surah (not a single verse) is picked at random each time this
  // screen renders, from CuratedAyahs (sourced from the app's own embedded Quran
  // text -- see that header for why a curated set instead of a true-random verse
  // extracted live from the Quran EPUB). Same on X3 and X4: both share the same
  // physical screen resolution (only RTC-clock availability and button layout
  // differ between them), so no device-specific sizing is needed here.
  int ayahBlockBottom = 20;
  if (QuranBook::isPinned()) {
    const auto& entry = CuratedAyahs::kEntries[random(static_cast<long>(CuratedAyahs::kCount))];
    const int ayahWidth = pageWidth - sidePadding * 2;
    const int ayahLineHeight = renderer.getLineHeight(UTHMANICHAFS_16_FONT_ID);
    // CuratedAyahs is curated to short surahs that fit in 2-3 lines; capped at
    // 3 as a safety margin so the card can never crowd the stat cards below.
    const auto ayahLines = renderer.wrappedText(UTHMANICHAFS_16_FONT_ID, entry.text, ayahWidth, /*maxLines=*/3);
    // Tajawal 8pt (not Uthmanic Hafs 12pt, the next size down from the ayah
    // body) -- 12pt only reads as "one step smaller"; 8pt reads as a genuinely
    // small caption the way a citation under a pull-quote should.
    const int refLineHeight = renderer.getLineHeight(TAJAWAL_8_FONT_ID);

    // Solid black card, white text -- reads as a distinct "screensaver" block
    // rather than more body copy, and a small reference under a larger ayah
    // is the usual quote-card convention.
    constexpr int kCardPaddingTop = 16;
    constexpr int kCardPaddingBottom = 14;
    constexpr int kCardGapBeforeRef = 6;
    const int cardHeight = kCardPaddingTop + static_cast<int>(ayahLines.size()) * (ayahLineHeight + 2) +
                           kCardGapBeforeRef + refLineHeight + kCardPaddingBottom;
    renderer.fillRect(0, 0, pageWidth, cardHeight, true);

    int ayahY = kCardPaddingTop;
    for (const auto& line : ayahLines) {
      const int lineWidth = renderer.getTextWidth(UTHMANICHAFS_16_FONT_ID, line.c_str());
      renderer.drawText(UTHMANICHAFS_16_FONT_ID, (pageWidth - lineWidth) / 2, ayahY, line.c_str(), false);
      ayahY += ayahLineHeight + 2;
    }
    ayahY += kCardGapBeforeRef;
    const int refWidth = renderer.getTextWidth(TAJAWAL_8_FONT_ID, entry.reference);
    renderer.drawText(TAJAWAL_8_FONT_ID, (pageWidth - refWidth) / 2, ayahY, entry.reference, false);
    ayahBlockBottom = cardHeight;
  }

  // Cover, top-left -- same 200x300 box the Home screen's hero card uses, drawn
  // straight from the book's full-resolution generated cover (drawBitmap only
  // ever scales down, so a single shrink-to-fit pass is enough).
  constexpr int kCoverWidth = 200;
  constexpr int kCoverHeight = 300;
  const int coverX = sidePadding;
  const int coverY = ayahBlockBottom + 20;

  bool hasCover = false;
  {
    HalFile file;
    // book.coverBmpPath is a TEMPLATE (Epub::getThumbBmpPath()'s "[HEIGHT]"
    // placeholder), not a real file path -- thumbnails only ever exist at the
    // Home screen's homeCoverHeight, so that's the only substitution that
    // resolves to a file actually on disk (drawBitmap scales the rest down).
    const std::string thumbPath =
        UITheme::getCoverThumbPath(book.coverBmpPath, UITheme::getInstance().getMetrics().homeCoverHeight);
    if (!thumbPath.empty() && Storage.openFileForRead("SLP", thumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        const float scale = std::min(static_cast<float>(kCoverWidth) / bitmap.getWidth(),
                                     static_cast<float>(kCoverHeight) / bitmap.getHeight());
        const int scaledW = static_cast<int>(bitmap.getWidth() * scale);
        const int scaledH = static_cast<int>(bitmap.getHeight() * scale);
        renderer.drawBitmap(bitmap, coverX + (kCoverWidth - scaledW) / 2, coverY + (kCoverHeight - scaledH) / 2,
                            kCoverWidth, kCoverHeight);
        hasCover = true;
      }
    }
  }
  if (!hasCover) {
    renderer.drawIcon(CoverIcon, coverX + (kCoverWidth - 32) / 2, coverY + kCoverHeight / 2 - 16, 32);
  }

  // Title / author / progress bar, to the right of the cover.
  const int metaX = coverX + kCoverWidth + 20;
  const int metaWidth = pageWidth - metaX - sidePadding;
  int textY = coverY + 6;

  const bool titleArabic = ScriptDetector::containsArabic(book.title.c_str());
  const int titleFontId = titleArabic ? TAJAWAL_18_FONT_ID : BITTER_18_FONT_ID;
  const int titleLineHeight = renderer.getLineHeight(titleFontId);
  const auto titleLines = renderer.wrappedText(titleFontId, book.title.c_str(), metaWidth, 3, EpdFontFamily::BOLD);
  for (const auto& line : titleLines) {
    renderer.drawTextInWidth(titleFontId, metaX, textY, metaWidth, line.c_str(), true, EpdFontFamily::BOLD);
    textY += titleLineHeight + 2;
  }
  textY += 6;

  if (!book.author.empty()) {
    const std::string author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), metaWidth);
    renderer.drawTextInWidth(UI_10_FONT_ID, metaX, textY, metaWidth, author.c_str());
    textY += renderer.getLineHeight(UI_10_FONT_ID) + 14;
  }

  {
    constexpr int kBarHeight = 10;
    char percentStr[8];
    snprintf(percentStr, sizeof(percentStr), "%d%%", progressPercent);
    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, percentStr, EpdFontFamily::BOLD);
    const int barW = std::max(0, metaWidth - labelW - 8);
    if (barW > 0) {
      renderer.drawRect(metaX, textY, barW, kBarHeight, true);
      const int fillW = std::clamp(barW * progressPercent / 100, 0, barW);
      if (fillW > 2) {
        renderer.fillRect(metaX + 1, textY + 1, fillW - 2, kBarHeight - 2, true);
      }
      renderer.drawText(UI_10_FONT_ID, metaX + barW + 8,
                        textY + (kBarHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2 - 1, percentStr, true,
                        EpdFontFamily::BOLD);
    }
  }

  // Stat cards: est. time left / current streak / total reading time / books
  // finished -- the same figures the in-app Stats screen shows (READING_STATS),
  // reusing AppMetricCard for visual consistency instead of hand-rolled boxes.
  constexpr int kCardHeight = 72;
  constexpr int kCardGap = 10;
  const int gridTop = coverY + kCoverHeight + 24;
  const int gridWidth = pageWidth - sidePadding * 2;
  const int cardWidth = (gridWidth - kCardGap) / 2;

  char leftBuf[24];
  if (estimatedLeftSeconds > 0 && progressPercent < 100) {
    formatReadingDuration(estimatedLeftSeconds, leftBuf, sizeof(leftBuf));
  } else {
    snprintf(leftBuf, sizeof(leftBuf), "-");
  }
  char streakBuf[16];
  snprintf(streakBuf, sizeof(streakBuf), "%lu", static_cast<unsigned long>(READING_STATS.getCurrentStreakDays()));
  char totalBuf[24];
  formatReadingDuration(static_cast<uint32_t>(READING_STATS.getTotalReadingMs() / 1000ULL), totalBuf,
                       sizeof(totalBuf));
  char finishedBuf[16];
  snprintf(finishedBuf, sizeof(finishedBuf), "%lu", static_cast<unsigned long>(READING_STATS.getBooksFinishedCount()));

  const auto cardAt = [&](const int row, const int col, const char* label, const char* value) {
    const Rect rect{sidePadding + col * (cardWidth + kCardGap), gridTop + row * (kCardHeight + kCardGap), cardWidth,
                    kCardHeight};
    AppMetricCard::draw(renderer, rect, label, value);
  };
  cardAt(0, 0, tr(STR_EST_LEFT), leftBuf);
  cardAt(0, 1, tr(STR_STREAK), streakBuf);
  cardAt(1, 0, tr(STR_READING_TIME), totalBuf);
  cardAt(1, 1, tr(STR_BOOKS_FINISHED), finishedBuf);

  // Footer: battery + (RTC devices only) current time.
  {
    char timeBuf[16] = "";
    halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1);
    char footer[48];
    if (timeBuf[0] != '\0') {
      snprintf(footer, sizeof(footer), "%s  \xc2\xb7  %u%%", timeBuf, powerManager.getBatteryPercentage());
    } else {
      snprintf(footer, sizeof(footer), "%s: %u%%", tr(STR_BATTERY), powerManager.getBatteryPercentage());
    }
    const int footerY = renderer.getScreenHeight() - renderer.getLineHeight(UI_10_FONT_ID) - 16;
    renderer.drawCenteredText(UI_10_FONT_ID, footerY, footer);
  }

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
