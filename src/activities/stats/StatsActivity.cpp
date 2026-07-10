#include "StatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ROW_HEIGHT = 34;

const char* bucketName(const size_t bucket) {
  switch (bucket) {
    case 0:
      return tr(STR_STATS_MORNING);
    case 1:
      return tr(STR_STATS_AFTERNOON);
    case 2:
      return tr(STR_STATS_EVENING);
    default:
      return tr(STR_STATS_NIGHT);
  }
}

const char* dayName(const size_t dayOfWeek) {
  static constexpr StrId kDays[] = {StrId::STR_DAY_MON, StrId::STR_DAY_TUE, StrId::STR_DAY_WED, StrId::STR_DAY_THU,
                                    StrId::STR_DAY_FRI, StrId::STR_DAY_SAT, StrId::STR_DAY_SUN};
  return I18N.get(kDays[dayOfWeek % 7]);
}
}  // namespace

void StatsActivity::onEnter() {
  Activity::onEnter();
  stats = GlobalReadingStats::load();
  today = getCurrentLocalReadingDateTime();
  requestUpdate();
}

void StatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void StatsActivity::drawRow(const int y, const char* label, const char* value) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  // Mirror label/value anchoring for RTL UI languages, matching BaseTheme::drawList.
  const bool rtl = I18N.isRtl();
  const int labelX = rtl ? pageWidth - metrics.contentSidePadding - renderer.getTextWidth(UI_10_FONT_ID, label)
                         : metrics.contentSidePadding;
  const int valueX = rtl ? metrics.contentSidePadding
                         : pageWidth - metrics.contentSidePadding - renderer.getTextWidth(UI_10_FONT_ID, value);
  renderer.drawText(UI_10_FONT_ID, labelX, y, label);
  renderer.drawText(UI_10_FONT_ID, valueX, y, value, true, EpdFontFamily::BOLD);
}

void StatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_STATS_TITLE));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  if (stats.totalReadingSeconds == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_STATS_NO_DATA));
  } else {
    char buf[48];

    formatReadingDuration(stats.totalReadingSeconds, buf, sizeof(buf));
    drawRow(y, tr(STR_STATS_TOTAL_TIME), buf);
    y += ROW_HEIGHT;

    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.totalSessions));
    drawRow(y, tr(STR_STATS_SESSIONS), buf);
    y += ROW_HEIGHT;

    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.totalPagesTurned));
    drawRow(y, tr(STR_STATS_PAGES), buf);
    y += ROW_HEIGHT;

    if (stats.totalPagesTurned > 0) {
      snprintf(buf, sizeof(buf), "%lus",
               static_cast<unsigned long>(stats.totalReadingSeconds / stats.totalPagesTurned));
      drawRow(y, tr(STR_STATS_AVG_PACE), buf);
      y += ROW_HEIGHT;
    }

    // Streak/history rows need date attribution, which needs a valid clock at least
    // once; historyAnchorDay stays 0 until then.
    if (stats.historyAnchorDay != 0) {
      const uint32_t todayIdx = today.valid ? today.dayIndex : 0;
      if (todayIdx != 0) {
        snprintf(buf, sizeof(buf), tr(STR_STATS_DAYS_FORMAT), static_cast<unsigned int>(stats.currentStreak(todayIdx)));
        drawRow(y, tr(STR_STATS_CURRENT_STREAK), buf);
        y += ROW_HEIGHT;
      }
      snprintf(buf, sizeof(buf), tr(STR_STATS_DAYS_FORMAT), static_cast<unsigned int>(stats.longestStreak));
      drawRow(y, tr(STR_STATS_LONGEST_STREAK), buf);
      y += ROW_HEIGHT;
      if (todayIdx != 0) {
        snprintf(buf, sizeof(buf), "%u / 30", static_cast<unsigned int>(stats.daysReadInLast(30, todayIdx)));
        drawRow(y, tr(STR_STATS_DAYS_LAST_30), buf);
        y += ROW_HEIGHT;
      }

      // Favorite time-of-day / most active weekday: the buckets with the most
      // accumulated seconds.
      const auto maxBucket = std::max_element(stats.timeOfDaySeconds.begin(), stats.timeOfDaySeconds.end()) -
                             stats.timeOfDaySeconds.begin();
      if (stats.timeOfDaySeconds[maxBucket] > 0) {
        drawRow(y, tr(STR_STATS_FAVORITE_TIME), bucketName(static_cast<size_t>(maxBucket)));
        y += ROW_HEIGHT;
      }
      const auto maxDay = std::max_element(stats.dayOfWeekSeconds.begin(), stats.dayOfWeekSeconds.end()) -
                          stats.dayOfWeekSeconds.begin();
      if (stats.dayOfWeekSeconds[maxDay] > 0) {
        drawRow(y, tr(STR_STATS_BUSIEST_DAY), dayName(static_cast<size_t>(maxDay)));
        y += ROW_HEIGHT;
      }

      // Last-14-days activity strip: one square per day, oldest on the left
      // (mirrored for RTL), filled when any reading happened that day.
      if (todayIdx != 0) {
        y += metrics.verticalSpacing;
        renderer.drawText(UI_10_FONT_ID,
                          I18N.isRtl() ? pageWidth - metrics.contentSidePadding -
                                             renderer.getTextWidth(UI_10_FONT_ID, tr(STR_STATS_LAST_14_DAYS))
                                       : metrics.contentSidePadding,
                          y, tr(STR_STATS_LAST_14_DAYS));
        y += ROW_HEIGHT;
        constexpr int kDaysShown = 14;
        constexpr int kSquare = 22;
        constexpr int kGap = 8;
        const int stripWidth = kDaysShown * kSquare + (kDaysShown - 1) * kGap;
        int x = (pageWidth - stripWidth) / 2;
        for (int i = kDaysShown - 1; i >= 0; i--) {
          const bool read = todayIdx >= static_cast<uint32_t>(i) && stats.wasDayRead(todayIdx - i);
          if (read) {
            renderer.fillRect(x, y, kSquare, kSquare);
          } else {
            renderer.drawRect(x, y, kSquare, kSquare);
          }
          x += kSquare + kGap;
        }
        y += kSquare;
      }
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
