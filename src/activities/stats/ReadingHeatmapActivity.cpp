#include "ReadingHeatmapActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include "AppMetricCard.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingDayDetailActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "reading/ReadingStatsStore.h"
#include "util/TimeUtils.h"

namespace {
constexpr int SECTION_GAP = 10;
constexpr int MONTH_HEADER_HEIGHT = 34;
constexpr int SUMMARY_CARD_HEIGHT = 64;
constexpr int SUMMARY_CARD_GAP = 8;
constexpr int GRID_GAP = 6;
constexpr int WEEKDAY_ROW_HEIGHT = 24;
constexpr int LEGEND_HEIGHT = 30;
constexpr int LEGEND_SWATCH_SIZE = 16;
constexpr int GRID_ROWS = 6;
constexpr int GRID_COLS = 7;
constexpr int GRID_CELLS = GRID_ROWS * GRID_COLS;

struct HeatmapCell {
  uint32_t dayOrdinal = 0;
  uint32_t readingMs = 0;
  unsigned day = 0;
  bool inViewedMonth = false;
  bool isToday = false;
  bool isSelected = false;
};

// Level 1 starts at "any reading at all" (cpr-vcodex left days under 15 minutes
// looking unread). Four levels, matching the renderer's four fill shades.
constexpr int HEAT_LEVEL_BLACK = 4;

int heatLevel(const uint32_t readingMs) {
  if (readingMs == 0) return 0;
  const uint32_t minutes = readingMs / 60000U;
  if (minutes < 30) return 1;
  if (minutes < 60) return 2;
  if (minutes < 120) return 3;
  return HEAT_LEVEL_BLACK;
}

void fillHeatShade(const GfxRenderer& renderer, const Rect& rect, const int level) {
  switch (level) {
    case 1:
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
      break;
    case 2:
    case 3:
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::DarkGray);
      break;
    case HEAT_LEVEL_BLACK:
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height);
      break;
    default:
      break;
  }
  if (level == 3) {
    // Distinguish level 3 from 2: DarkGray dither plus a heavy inner border.
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
    renderer.drawRect(rect.x + 1, rect.y + 1, std::max(0, rect.width - 2), std::max(0, rect.height - 2));
  }
}

void drawGoalCheckBadge(const GfxRenderer& renderer, const Rect& rect, const bool darkBackground) {
  constexpr int checkWidth = 20;
  constexpr int checkHeight = 16;
  const int checkX = rect.x + rect.width - checkWidth - 7;
  const int checkY = rect.y + rect.height - checkHeight - 7;
  const bool checkColor = !darkBackground;
  renderer.drawLine(checkX, checkY + 8, checkX + 5, checkY + 13, 4, checkColor);
  renderer.drawLine(checkX + 5, checkY + 13, checkX + 17, checkY + 1, 4, checkColor);
}

void drawHeatCell(const GfxRenderer& renderer, const Rect& rect, const HeatmapCell& cell) {
  const int level = cell.inViewedMonth ? heatLevel(cell.readingMs) : 0;
  fillHeatShade(renderer, Rect{rect.x + 1, rect.y + 1, std::max(0, rect.width - 2), std::max(0, rect.height - 2)},
                level);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  if (cell.day != 0) {
    char dayText[4];
    snprintf(dayText, sizeof(dayText), "%u", cell.day);
    renderer.drawText(SMALL_FONT_ID, rect.x + 6, rect.y + 5, dayText, level < 4,
                      cell.inViewedMonth ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  if (cell.inViewedMonth && cell.readingMs > 0 && static_cast<uint64_t>(cell.readingMs) >= SETTINGS.getDailyGoalMs()) {
    drawGoalCheckBadge(renderer, rect, level >= 4);
  }

  if (cell.isToday) {
    renderer.drawRect(rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4, level < 4);
  }
  if (cell.isSelected) {
    renderer.drawRect(rect.x + 4, rect.y + 4, std::max(0, rect.width - 8), std::max(0, rect.height - 8), level < 4);
  }
}

std::array<HeatmapCell, GRID_CELLS> buildCells(const int year, const unsigned month, const uint32_t todayOrdinal,
                                               const uint32_t selectedDayOrdinal) {
  std::array<HeatmapCell, GRID_CELLS> cells{};
  const uint32_t firstDayOrdinal = TimeUtils::getDayOrdinalForDate(year, month, 1);
  const int firstWeekday = TimeUtils::weekdayForOrdinal(firstDayOrdinal);  // Monday = 0
  const uint32_t gridStartOrdinal = firstDayOrdinal - static_cast<uint32_t>(firstWeekday);

  for (size_t index = 0; index < cells.size(); ++index) {
    auto& cell = cells[index];
    cell.dayOrdinal = gridStartOrdinal + static_cast<uint32_t>(index);
    int cellYear = 0;
    unsigned cellMonth = 0;
    unsigned cellDay = 0;
    TimeUtils::getDateFromDayOrdinal(cell.dayOrdinal, cellYear, cellMonth, cellDay);
    cell.day = cellDay;
    cell.inViewedMonth = cellYear == year && cellMonth == month;
    cell.isToday = cell.inViewedMonth && todayOrdinal != 0 && cell.dayOrdinal == todayOrdinal;
    cell.isSelected = cell.inViewedMonth && selectedDayOrdinal != 0 && cell.dayOrdinal == selectedDayOrdinal;
    cell.readingMs = READING_STATS.readingMsOnDay(cell.dayOrdinal);
  }
  return cells;
}

const char* weekdayShortName(const int weekday) {
  static constexpr StrId kDays[7] = {StrId::STR_WD_MON, StrId::STR_WD_TUE, StrId::STR_WD_WED, StrId::STR_WD_THU,
                                     StrId::STR_WD_FRI, StrId::STR_WD_SAT, StrId::STR_WD_SUN};
  return I18N.get(kDays[weekday % 7]);
}

// Reference day for "today": the real clock when valid, else the newest
// recorded reading day so the heatmap still opens somewhere sensible.
uint32_t referenceDayOrdinal() {
  const uint32_t today = TimeUtils::todayOrdinal();
  return today != 0 ? today : READING_STATS.getLatestReadingDayOrdinal();
}
}  // namespace

void ReadingHeatmapActivity::onEnter() {
  Activity::onEnter();
  READING_STATS.ensureLoaded();

  int year = 2026;
  unsigned month = 1;
  unsigned day = 1;
  const uint32_t reference = referenceDayOrdinal();
  if (reference == 0 || !TimeUtils::getDateFromDayOrdinal(reference, year, month, day)) {
    year = 2026;
    month = 1;
  }
  viewedYear = year;
  viewedMonth = month;
  resetSelectedDay();
  waitForConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

void ReadingHeatmapActivity::resetSelectedDay() {
  const uint32_t reference = referenceDayOrdinal();
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (reference != 0 && TimeUtils::getDateFromDayOrdinal(reference, year, month, day) && year == viewedYear &&
      month == viewedMonth) {
    selectedDayOrdinal = reference;
    return;
  }
  selectedDayOrdinal = TimeUtils::getDayOrdinalForDate(viewedYear, viewedMonth, 1);
}

void ReadingHeatmapActivity::goToAdjacentMonth(const int delta) {
  unsigned preferredDay = 1;
  if (selectedDayOrdinal != 0) {
    int year = 0;
    unsigned month = 0;
    TimeUtils::getDateFromDayOrdinal(selectedDayOrdinal, year, month, preferredDay);
  }
  int newMonth = static_cast<int>(viewedMonth) + delta;
  int newYear = viewedYear;
  while (newMonth < 1) {
    newMonth += 12;
    newYear--;
  }
  while (newMonth > 12) {
    newMonth -= 12;
    newYear++;
  }
  viewedYear = newYear;
  viewedMonth = static_cast<unsigned>(newMonth);
  const unsigned clampedDay = std::min(std::max(preferredDay, 1U), TimeUtils::daysInMonth(viewedYear, viewedMonth));
  selectedDayOrdinal = TimeUtils::getDayOrdinalForDate(viewedYear, viewedMonth, clampedDay);
  requestUpdate();
}

void ReadingHeatmapActivity::moveSelection(const int delta) {
  if (selectedDayOrdinal == 0) {
    resetSelectedDay();
    requestUpdate();
    return;
  }
  const int64_t next = static_cast<int64_t>(selectedDayOrdinal) + delta;
  if (next < 1) {
    return;
  }
  selectedDayOrdinal = static_cast<uint32_t>(next);
  // Crossing a month edge follows the selection into the adjacent month.
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (TimeUtils::getDateFromDayOrdinal(selectedDayOrdinal, year, month, day) &&
      (year != viewedYear || month != viewedMonth)) {
    viewedYear = year;
    viewedMonth = month;
  }
  requestUpdate();
}

void ReadingHeatmapActivity::loop() {
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
    if (selectedDayOrdinal != 0) {
      startActivityForResult(std::make_unique<ReadingDayDetailActivity>(renderer, mappedInput, selectedDayOrdinal),
                             [this](const ActivityResult&) { requestUpdate(); });
    }
    return;
  }

  // In RTL the horizontal buttons move with the mirrored calendar.
  const int horizontalStep = I18N.isRtl() ? -1 : 1;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this, horizontalStep] { moveSelection(-horizontalStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this, horizontalStep] { moveSelection(horizontalStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { goToAdjacentMonth(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { goToAdjacentMonth(1); });
}

void ReadingHeatmapActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int sidePadding = metrics.contentSidePadding;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const bool rtl = I18N.isRtl();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_HEATMAP));

  // Month sub-header: month name + selected date.
  char monthLabel[24];
  TimeUtils::formatMonthYear(viewedYear, viewedMonth, monthLabel, sizeof(monthLabel));
  char selectedLabel[28];
  TimeUtils::formatDayOrdinal(selectedDayOrdinal, selectedLabel, sizeof(selectedLabel));
  GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, MONTH_HEADER_HEIGHT}, monthLabel,
                    selectedLabel[0] != '\0' ? selectedLabel : nullptr);

  // Month summary: total time / days read / best day / current streak.
  uint64_t monthTotalMs = 0;
  uint32_t monthDaysRead = 0;
  uint32_t bestDayMs = 0;
  unsigned bestDayOfMonth = 0;
  const uint32_t monthStart = TimeUtils::getDayOrdinalForDate(viewedYear, viewedMonth, 1);
  const uint32_t monthEnd =
      TimeUtils::getDayOrdinalForDate(viewedYear, viewedMonth, TimeUtils::daysInMonth(viewedYear, viewedMonth));
  for (const auto& day : READING_STATS.getReadingDays()) {
    if (day.dayOrdinal < monthStart || day.dayOrdinal > monthEnd || day.readingMs == 0) {
      continue;
    }
    monthTotalMs += day.readingMs;
    monthDaysRead++;
    if (day.readingMs > bestDayMs) {
      bestDayMs = day.readingMs;
      bestDayOfMonth = static_cast<unsigned>(day.dayOrdinal - monthStart + 1);
    }
  }

  const int summaryTop = contentTop + MONTH_HEADER_HEIGHT + 4;
  const int cardWidth = (pageWidth - sidePadding * 2 - SUMMARY_CARD_GAP) / 2;
  char value[32];
  const auto cardAt = [&](const int row, const int col, const char* label, const char* text) {
    const int slot = rtl ? 1 - col : col;
    const Rect rect{sidePadding + slot * (cardWidth + SUMMARY_CARD_GAP),
                    summaryTop + row * (SUMMARY_CARD_HEIGHT + SUMMARY_CARD_GAP), cardWidth, SUMMARY_CARD_HEIGHT};
    AppMetricCard::Options options;
    options.valueY = 10;
    options.labelY = 38;
    AppMetricCard::draw(renderer, rect, label, text, options);
  };

  formatReadingDuration(static_cast<uint32_t>(monthTotalMs / 1000ULL), value, sizeof(value));
  cardAt(0, 0, tr(STR_MONTH_TOTAL), value);
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(monthDaysRead));
  cardAt(0, 1, tr(STR_DAYS_READ), value);
  if (bestDayOfMonth > 0) {
    char bestBuf[24];
    formatReadingDuration(bestDayMs / 1000U, bestBuf, sizeof(bestBuf));
    snprintf(value, sizeof(value), "%s (%u)", bestBuf, bestDayOfMonth);
  } else {
    snprintf(value, sizeof(value), "-");
  }
  cardAt(1, 0, tr(STR_BEST_DAY), value);
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(READING_STATS.getCurrentStreakDays()));
  cardAt(1, 1, tr(STR_STREAK), value);

  // Calendar grid with a weekday header row; columns mirror in RTL.
  const int weekdayTop = summaryTop + (SUMMARY_CARD_HEIGHT + SUMMARY_CARD_GAP) * 2 + SECTION_GAP;
  const int gridTop = weekdayTop + WEEKDAY_ROW_HEIGHT;
  const int legendTop = pageHeight - metrics.buttonHintsHeight - LEGEND_HEIGHT - 4;
  const int gridHeight = std::max(120, legendTop - gridTop - SECTION_GAP);
  const int cellWidth = (pageWidth - sidePadding * 2 - GRID_GAP * (GRID_COLS - 1)) / GRID_COLS;
  const int cellHeight = (gridHeight - GRID_GAP * (GRID_ROWS - 1)) / GRID_ROWS;
  const auto columnX = [&](const int col) {
    const int slot = rtl ? GRID_COLS - 1 - col : col;
    return sidePadding + slot * (cellWidth + GRID_GAP);
  };

  for (int col = 0; col < GRID_COLS; ++col) {
    const char* name = weekdayShortName(col);
    const int x = columnX(col) + (cellWidth - renderer.getTextWidth(SMALL_FONT_ID, name, EpdFontFamily::BOLD)) / 2;
    renderer.drawText(SMALL_FONT_ID, x, weekdayTop, name, true, EpdFontFamily::BOLD);
  }

  const auto cells = buildCells(viewedYear, viewedMonth, TimeUtils::todayOrdinal(), selectedDayOrdinal);
  for (int index = 0; index < GRID_CELLS; ++index) {
    const int row = index / GRID_COLS;
    const int y = gridTop + row * (cellHeight + GRID_GAP);
    drawHeatCell(renderer, Rect{columnX(index % GRID_COLS), y, cellWidth, cellHeight},
                 cells[static_cast<size_t>(index)]);
  }

  // Legend (mirrors in RTL alongside everything else).
  static constexpr const char* LEGEND_LABELS[4] = {"1m+", "30m+", "1h+", "2h+"};
  const int itemWidth = (pageWidth - sidePadding * 2) / 4;
  for (int index = 0; index < 4; ++index) {
    const int slot = rtl ? 3 - index : index;
    const int itemX = sidePadding + slot * itemWidth;
    const Rect swatch{itemX + 6, legendTop + 3, LEGEND_SWATCH_SIZE, LEGEND_SWATCH_SIZE};
    fillHeatShade(renderer, Rect{swatch.x + 1, swatch.y + 1, swatch.width - 2, swatch.height - 2}, index + 1);
    renderer.drawRect(swatch.x, swatch.y, swatch.width, swatch.height);
    renderer.drawText(SMALL_FONT_ID, itemX + 28, legendTop + 6, LEGEND_LABELS[index]);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
