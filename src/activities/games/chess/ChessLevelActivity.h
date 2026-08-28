#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// "New Game": pick an opponent persona and which side to play, then hand both
// back through ChessSetupResult.
class ChessLevelActivity final : public Activity {
 public:
  explicit ChessLevelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ChessLevel", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  const char* activityDebugName() const override { return "ChessLevelActivity"; }

 private:
  enum class Side : uint8_t { White = 0, Black = 1, Random = 2 };
  static constexpr int SIDE_COUNT = 3;

  ButtonNavigator buttonNavigator_;
  int selectedLevel_ = 2;  // Omar, the middle persona
  Side side_ = Side::White;
  bool sideRowFocused_ = false;
  // The side row opens a drill-down list rather than cycling in place. Cycling
  // needed Left/Right, which are the only two front buttons this screen has left
  // once Back and Confirm are spoken for, and it hid the options that were not
  // currently picked -- you had to press through them to find out what existed.
  bool sideListOpen_ = false;
  int sideListIndex_ = 0;

  const char* sideLabel(int index) const;
  void confirmSelection();
  void renderSideList() const;
};
