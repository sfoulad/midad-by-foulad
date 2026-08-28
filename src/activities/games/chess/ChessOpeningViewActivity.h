#pragma once

#include <memory>

#include "ChessBoardView.h"
#include "ChessGame.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Read-only walkthrough of one opening: step forward and back through the line
// with the board, the move list and a coaching tip on the plies that have one.
class ChessOpeningViewActivity final : public Activity {
 public:
  ChessOpeningViewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int lineIndex)
      : Activity("ChessOpeningView", renderer, mappedInput), lineIndex_(lineIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  const char* activityDebugName() const override { return "ChessOpeningViewActivity"; }

 private:
  // Replays the line from the start up to `ply`. Cheap at these lengths and it
  // keeps a single source of truth for the position: the SAN in the book.
  void gotoPly(int ply);
  void drawMoveChips(int y, int width) const;

  std::unique_ptr<chess::Game> game_;
  ButtonNavigator buttonNavigator_;
  chess_view::Layout layout_{};
  int lineIndex_ = 0;
  int ply_ = 0;
  int totalPlies_ = 0;
};
