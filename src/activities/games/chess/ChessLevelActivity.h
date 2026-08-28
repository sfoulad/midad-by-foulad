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

  ButtonNavigator buttonNavigator_;
  int selectedLevel_ = 2;  // Omar, the middle persona
  Side side_ = Side::White;
  bool sideRowFocused_ = false;

  void confirmSelection();
};
