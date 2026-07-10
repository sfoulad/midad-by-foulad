#pragma once
#include "activities/Activity.h"
#include "reading/ReadingStats.h"

// Global reading statistics screen (Home -> Stats). Read-only view over
// GlobalReadingStats; all data is loaded once in onEnter.
class StatsActivity final : public Activity {
  GlobalReadingStats stats;
  ReadingLocalDateTime today;

  void drawRow(int y, const char* label, const char* value) const;

 public:
  explicit StatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Stats", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
