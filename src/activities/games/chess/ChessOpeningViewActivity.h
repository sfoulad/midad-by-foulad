#pragma once

#include <memory>
#include <string>
#include <vector>

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
  void renderTipPage() const;

  // The opened tip is a panel sized to its own text, not a full page: most notes
  // are two or three lines and a screen-tall block of white around them reads as a
  // rendering fault. Long ones stop at what fits and scroll instead. Measured in one
  // place because the scroll clamp in loop() and the panel in render() have to agree
  // about how many lines fit.
  struct TipLayout {
    std::vector<std::string> lines;
    int shownLines = 0;  // how many of them the panel has room for
    int maxScroll = 0;   // first line the panel may start at
    int panelHeight = 0;
    int panelY = 0;
  };
  TipLayout tipLayout() const;

  // Same three-step axis as the game board: the tip block sits below the board, so
  // Down steps onto it and Confirm opens it. The inline block is capped at four
  // lines by the space under the board, which truncates the longer coaching notes;
  // the opened page has the whole screen and shows them in full.
  enum class Focus : uint8_t { Board, Tip, TipReading };

  std::unique_ptr<chess::Game> game_;
  ButtonNavigator buttonNavigator_;
  chess_view::Layout layout_{};
  Focus focus_ = Focus::Board;
  int lineIndex_ = 0;
  int ply_ = 0;
  int totalPlies_ = 0;
  // First body line drawn in the opened tip. Reset whenever one is opened, so every
  // note starts at its beginning rather than wherever the last one was left.
  int tipScroll_ = 0;
};
