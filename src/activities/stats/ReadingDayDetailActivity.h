#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Per-day breakdown (Heatmap -> Confirm on a day): total time and book count
// cards, then the books read that day with their per-day time, sorted by time.
// Confirm opens the selected book in the reader. Ported from cpr-vcodex's
// ReadingDayDetailActivity.
class ReadingDayDetailActivity final : public Activity {
  struct DayBookEntry {
    std::string path;
    std::string title;
    std::string author;
    uint32_t readingMs = 0;
  };

  const uint32_t dayOrdinal;
  std::vector<DayBookEntry> entries;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool waitForConfirmRelease = false;

  void refreshEntries();

 public:
  explicit ReadingDayDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint32_t dayOrdinal)
      : Activity("ReadingDayDetail", renderer, mappedInput), dayOrdinal(dayOrdinal) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
