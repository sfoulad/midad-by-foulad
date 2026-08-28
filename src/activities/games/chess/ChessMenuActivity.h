#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Chess landing screen: new game, resume and the openings trainer.
// Reached from the Games menu, and the parent of every other chess screen.
class ChessMenuActivity final : public Activity {
 public:
  explicit ChessMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ChessMenu", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  const char* activityDebugName() const override { return "ChessMenuActivity"; }

 private:
  // Continue is hidden with no saved game, so the row indices shift; every
  // lookup goes through rowKindAt() rather than a hardcoded index.
  enum class Row { NewGame, Continue, Openings };

  ButtonNavigator buttonNavigator_;
  int selectedIndex_ = 0;
  bool hasSaved_ = false;

  int rowCount() const { return hasSaved_ ? 3 : 2; }
  Row rowKindAt(int index) const;
  void launchSelected();
  void startGame(int level, bool playerIsWhite, bool resume);
};
