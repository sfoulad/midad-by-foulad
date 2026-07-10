#include "ReadingDayDetailActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "AppMetricCard.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "reading/ReadingStatsStore.h"
#include "util/TimeUtils.h"

namespace {
constexpr int SUMMARY_CARD_HEIGHT = 64;
constexpr int SUMMARY_GAP = 8;
// Books with under 3 minutes on the day are noise, not "read that day".
constexpr uint32_t MIN_DAY_BOOK_MS = 3U * 60U * 1000U;
}  // namespace

void ReadingDayDetailActivity::refreshEntries() {
  entries.clear();
  const auto& books = READING_STATS.getBooks();
  entries.reserve(books.size());
  for (const auto& book : books) {
    const auto it = std::find_if(book.readingDays.begin(), book.readingDays.end(),
                                 [this](const ReadingDayStats& day) { return day.dayOrdinal == dayOrdinal; });
    if (it == book.readingDays.end() || it->readingMs < MIN_DAY_BOOK_MS) {
      continue;
    }
    entries.push_back(DayBookEntry{book.path, book.title.empty() ? book.path : book.title, book.author, it->readingMs});
  }
  std::sort(entries.begin(), entries.end(),
            [](const DayBookEntry& a, const DayBookEntry& b) { return a.readingMs > b.readingMs; });
  if (selectedIndex >= static_cast<int>(entries.size())) {
    selectedIndex = std::max(0, static_cast<int>(entries.size()) - 1);
  }
}

void ReadingDayDetailActivity::onEnter() {
  Activity::onEnter();
  READING_STATS.ensureLoaded();
  refreshEntries();
  waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

void ReadingDayDetailActivity::loop() {
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
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(entries.size())) {
      activityManager.goToReader(entries[selectedIndex].path);
    }
    return;
  }
  buttonNavigator.onNextRelease([this] {
    if (entries.empty()) return;
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(entries.size()));
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    if (entries.empty()) return;
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(entries.size()));
    requestUpdate();
  });
}

void ReadingDayDetailActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int sidePadding = metrics.contentSidePadding;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int cardWidth = (pageWidth - sidePadding * 2 - SUMMARY_GAP) / 2;
  const bool rtl = I18N.isRtl();

  char dateLabel[28];
  TimeUtils::formatDayOrdinal(dayOrdinal, dateLabel, sizeof(dateLabel));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_DAY),
                 dateLabel[0] != '\0' ? dateLabel : nullptr);

  char value[32];
  const auto cardAt = [&](const int col, const char* label, const char* text) {
    const int slot = rtl ? 1 - col : col;
    const Rect rect{sidePadding + slot * (cardWidth + SUMMARY_GAP), contentTop, cardWidth, SUMMARY_CARD_HEIGHT};
    AppMetricCard::Options options;
    options.valueY = 10;
    options.labelY = 38;
    AppMetricCard::draw(renderer, rect, label, text, options);
  };

  formatReadingDuration(READING_STATS.readingMsOnDay(dayOrdinal) / 1000U, value, sizeof(value));
  cardAt(0, tr(STR_TOTAL_TIME), value);
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(entries.size()));
  cardAt(1, tr(STR_BOOKS_READ), value);

  const int listTop = contentTop + SUMMARY_CARD_HEIGHT + metrics.verticalSpacing;
  const char* topBookTitle = entries.empty() ? "-" : entries.front().title.c_str();
  GUI.drawSubHeader(renderer, Rect{0, listTop, pageWidth, 34}, tr(STR_TOP_BOOK), topBookTitle);

  const int listContentTop = listTop + 34 + 8;
  const int listHeight = pageHeight - listContentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, listContentTop + 24, tr(STR_STATS_NO_DATA));
  } else {
    GUI.drawList(
        renderer, Rect{0, listContentTop, pageWidth, listHeight}, static_cast<int>(entries.size()), selectedIndex,
        [this](const int index) { return entries[index].title; },
        [this](const int index) {
          return entries[index].author.empty() ? std::string(tr(STR_IN_PROGRESS)) : entries[index].author;
        },
        [](const int) { return UIIcon::Book; },
        [this](const int index) {
          char buf[24];
          formatReadingDuration(entries[index].readingMs / 1000U, buf, sizeof(buf));
          return std::string(buf);
        });
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), entries.empty() ? "" : tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
