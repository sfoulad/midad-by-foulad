#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// List of the built-in openings; Confirm opens the walkthrough.
class ChessOpeningsActivity final : public Activity {
 public:
  explicit ChessOpeningsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ChessOpenings", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  const char* activityDebugName() const override { return "ChessOpeningsActivity"; }

 private:
  ButtonNavigator buttonNavigator_;
  int selectedIndex_ = 0;
};
