#include "StatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "AppMetricCard.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingHeatmapActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "reading/ReadingStatsStore.h"

namespace {
constexpr unsigned long BOOK_LONG_PRESS_MS = 1000;
constexpr int SUMMARY_CARD_HEIGHT = 72;
constexpr int SUMMARY_GAP = 10;
constexpr int HEATMAP_BUTTON_HEIGHT = 54;
constexpr int LIST_HEADER_HEIGHT = 34;
constexpr int LIST_HEADER_BOTTOM_GAP = 8;
constexpr int BOOK_ROW_HEIGHT = 78;
constexpr int BOOK_ROW_GAP = 8;
constexpr int BOOKS_PER_PAGE = 3;

const char* bookTitle(const ReadingBookStats& book) {
  return book.title.empty() ? book.path.c_str() : book.title.c_str();
}

void drawHeatmapButton(const GfxRenderer& renderer, const Rect& rect, const bool selected) {
  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  const char* label = tr(STR_READING_HEATMAP);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2 + 2;
  renderer.drawText(UI_12_FONT_ID, rect.x + (rect.width - textWidth) / 2, textY, label, true, EpdFontFamily::BOLD);
}

void drawMiniProgressBar(const GfxRenderer& renderer, const Rect& rect, const uint8_t percent, const bool rtl) {
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  const int innerWidth = std::max(0, rect.width - 4);
  const int fillWidth = innerWidth * std::min<int>(percent, 100) / 100;
  if (fillWidth > 0) {
    const int fillX = rtl ? rect.x + 2 + innerWidth - fillWidth : rect.x + 2;
    renderer.fillRect(fillX, rect.y + 2, fillWidth, std::max(0, rect.height - 4));
  }
}

void drawBookRow(const GfxRenderer& renderer, const Rect& rect, const ReadingBookStats& book, const bool selected,
                 const bool rtl) {
  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  } else {
    renderer.drawLine(rect.x, rect.y + rect.height, rect.x + rect.width, rect.y + rect.height);
  }

  constexpr int sidePadding = 12;
  constexpr int metaWidth = 88;
  const int titleY = rect.y + 8;
  const int subtitleY = titleY + 26;
  const int textWidth = rect.width - sidePadding * 2 - metaWidth;
  // Text column and meta column swap sides in RTL.
  const int textLeft = rtl ? rect.x + sidePadding + metaWidth : rect.x + sidePadding;

  const std::string title = renderer.truncatedText(UI_12_FONT_ID, bookTitle(book), textWidth - 4, EpdFontFamily::BOLD);
  const std::string subtitle = renderer.truncatedText(
      UI_10_FONT_ID, book.author.empty() ? (book.completed ? tr(STR_DONE) : tr(STR_IN_PROGRESS)) : book.author.c_str(),
      textWidth - 4);
  const int titleX =
      rtl ? textLeft + textWidth - renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD) : textLeft;
  const int subtitleX = rtl ? textLeft + textWidth - renderer.getTextWidth(UI_10_FONT_ID, subtitle.c_str()) : textLeft;
  renderer.drawText(UI_12_FONT_ID, titleX, titleY, title.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, subtitleX, subtitleY, subtitle.c_str());

  char progressText[8];
  snprintf(progressText, sizeof(progressText), "%u%%", static_cast<unsigned>(book.lastProgressPercent));
  char timeText[24];
  formatReadingDuration(static_cast<uint32_t>(book.totalReadingMs / 1000ULL), timeText, sizeof(timeText));
  const int progressX =
      rtl ? rect.x + sidePadding
          : rect.x + rect.width - sidePadding - renderer.getTextWidth(UI_12_FONT_ID, progressText, EpdFontFamily::BOLD);
  const int timeX =
      rtl ? rect.x + sidePadding : rect.x + rect.width - sidePadding - renderer.getTextWidth(UI_10_FONT_ID, timeText);
  renderer.drawText(UI_12_FONT_ID, progressX, titleY, progressText, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, timeX, subtitleY, timeText);

  drawMiniProgressBar(renderer, Rect{rect.x + sidePadding, rect.y + rect.height - 14, rect.width - sidePadding * 2, 9},
                      book.lastProgressPercent, rtl);
}
}  // namespace

void StatsActivity::onEnter() {
  Activity::onEnter();
  READING_STATS.ensureLoaded();
  selectedIndex = 0;
  waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  waitForBackRelease = false;
  requestUpdate();
}

void StatsActivity::loop() {
  const int bookCount = static_cast<int>(READING_STATS.getBooks().size());
  const int selectableCount = bookCount + 1;

  if (waitForBackRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      waitForBackRelease = false;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (waitForConfirmRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      waitForConfirmRelease = false;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex > 0 && mappedInput.getHeldTime() >= BOOK_LONG_PRESS_MS) {
      confirmRemoveSelectedBook();
    } else {
      openSelectedEntry();
    }
    return;
  }

  buttonNavigator.onScrollNextRelease([this, selectableCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, selectableCount);
    requestUpdate();
  });
  buttonNavigator.onScrollPreviousRelease([this, selectableCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, selectableCount);
    requestUpdate();
  });
  buttonNavigator.onScrollNextContinuous([this, selectableCount] {
    if (selectableCount <= 1) return;
    const int bookIndex = std::max(0, selectedIndex - 1);
    selectedIndex = ButtonNavigator::nextPageIndex(bookIndex, selectableCount - 1, BOOKS_PER_PAGE) + 1;
    requestUpdate();
  });
  buttonNavigator.onScrollPreviousContinuous([this, selectableCount] {
    if (selectableCount <= 1) return;
    const int bookIndex = std::max(0, selectedIndex - 1);
    selectedIndex = ButtonNavigator::previousPageIndex(bookIndex, selectableCount - 1, BOOKS_PER_PAGE) + 1;
    requestUpdate();
  });
}

void StatsActivity::openSelectedEntry() {
  const auto& books = READING_STATS.getBooks();
  if (selectedIndex == 0) {
    startActivityForResult(std::make_unique<ReadingHeatmapActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) {
                             waitForBackRelease = true;
                             requestUpdate();
                           });
    return;
  }
  const int bookIndex = selectedIndex - 1;
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) {
    return;
  }
  // Jump straight into reading the selected book.
  activityManager.goToReader(books[bookIndex].path);
}

void StatsActivity::confirmRemoveSelectedBook() {
  const auto& books = READING_STATS.getBooks();
  const int bookIndex = selectedIndex - 1;
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) {
    return;
  }
  const std::string path = books[bookIndex].path;
  const std::string title = bookTitle(books[bookIndex]);
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_STATS_ENTRY), title),
      [this, path](const ActivityResult& result) {
        if (!result.isCancelled && READING_STATS.removeBook(path)) {
          const int bookCount = static_cast<int>(READING_STATS.getBooks().size());
          selectedIndex = std::min(selectedIndex, bookCount);
        }
        waitForBackRelease = true;
        requestUpdate(true);
      });
}

void StatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int sidePadding = metrics.contentSidePadding;
  const int cardWidth = (pageWidth - sidePadding * 2 - SUMMARY_GAP) / 2;
  const int summaryTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const bool rtl = I18N.isRtl();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_STATS_TITLE));

  char value[48];
  char goalBuf[24];
  const uint64_t todayMs = READING_STATS.getTodayReadingMs();
  const uint64_t goalMs = SETTINGS.getDailyGoalMs();
  const auto cardAt = [&](const int row, const int col, const char* label, const char* text, const bool check) {
    // Column order mirrors in RTL.
    const int slot = rtl ? 1 - col : col;
    const Rect rect{sidePadding + slot * (cardWidth + SUMMARY_GAP),
                    summaryTop + row * (SUMMARY_CARD_HEIGHT + SUMMARY_GAP), cardWidth, SUMMARY_CARD_HEIGHT};
    AppMetricCard::Options options;
    options.showCheck = check;
    AppMetricCard::draw(renderer, rect, label, text, options);
  };

  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(READING_STATS.getCurrentStreakDays()));
  cardAt(0, 0, tr(STR_STREAK), value, false);
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(READING_STATS.getMaxStreakDays()));
  cardAt(0, 1, tr(STR_MAX_STREAK), value, false);

  formatReadingDuration(static_cast<uint32_t>(todayMs / 1000ULL), goalBuf, sizeof(goalBuf));
  char goalTarget[24];
  formatReadingDuration(static_cast<uint32_t>(goalMs / 1000ULL), goalTarget, sizeof(goalTarget));
  snprintf(value, sizeof(value), "%s / %s", goalBuf, goalTarget);
  cardAt(1, 0, tr(STR_DAILY_GOAL), value, todayMs >= goalMs && todayMs > 0);
  formatReadingDuration(static_cast<uint32_t>(READING_STATS.getTotalReadingMs() / 1000ULL), value, sizeof(value));
  cardAt(1, 1, tr(STR_READING_TIME), value, false);

  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(READING_STATS.getBooksFinishedCount()));
  cardAt(2, 0, tr(STR_BOOKS_FINISHED), value, false);
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(READING_STATS.getBooksStartedCount()));
  cardAt(2, 1, tr(STR_BOOKS_STARTED), value, false);

  const int buttonTop = summaryTop + SUMMARY_CARD_HEIGHT * 3 + SUMMARY_GAP * 2 + metrics.verticalSpacing;
  drawHeatmapButton(renderer, Rect{sidePadding, buttonTop, pageWidth - sidePadding * 2, HEATMAP_BUTTON_HEIGHT},
                    selectedIndex == 0);

  const auto& books = READING_STATS.getBooks();
  const int listHeaderTop = buttonTop + HEATMAP_BUTTON_HEIGHT + metrics.verticalSpacing;
  const int totalPages = std::max(1, static_cast<int>((books.size() + BOOKS_PER_PAGE - 1) / BOOKS_PER_PAGE));
  const int currentPage = (books.empty() || selectedIndex == 0) ? 1 : ((selectedIndex - 1) / BOOKS_PER_PAGE) + 1;
  char headerLabel[64];
  snprintf(headerLabel, sizeof(headerLabel), "%s (%lu)", tr(STR_STARTED_BOOKS),
           static_cast<unsigned long>(books.size()));
  char pageLabel[16];
  snprintf(pageLabel, sizeof(pageLabel), "%d/%d", currentPage, totalPages);
  GUI.drawSubHeader(renderer, Rect{0, listHeaderTop, pageWidth, LIST_HEADER_HEIGHT}, headerLabel, pageLabel);

  const int contentTop = listHeaderTop + LIST_HEADER_HEIGHT + LIST_HEADER_BOTTOM_GAP;
  if (books.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 24, tr(STR_STATS_NO_DATA));
  } else {
    const int selectedBookIndex = std::max(0, selectedIndex - 1);
    const int pageStartIndex = (selectedBookIndex / BOOKS_PER_PAGE) * BOOKS_PER_PAGE;
    const int pageEndIndex = std::min(static_cast<int>(books.size()), pageStartIndex + BOOKS_PER_PAGE);
    for (int index = pageStartIndex; index < pageEndIndex; ++index) {
      const int rowY = contentTop + (index - pageStartIndex) * (BOOK_ROW_HEIGHT + BOOK_ROW_GAP);
      drawBookRow(renderer, Rect{sidePadding, rowY, pageWidth - sidePadding * 2, BOOK_ROW_HEIGHT}, books[index],
                  selectedIndex == index + 1, rtl);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN), /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
