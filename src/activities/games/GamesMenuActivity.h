#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Small picker shown when the user taps the "Games" tile in My Books (see
// GAMES_PSEUDO_PATH in RecentBooksActivity.cpp) -- lets them choose between
// the built-in Snake, Tetris, Sudoku, Maze and Chess games.
class GamesMenuActivity final : public Activity {
 public:
  explicit GamesMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GamesMenu", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Game { SNAKE = 0, TETRIS = 1, SUDOKU = 2, MAZE = 3, CHESS = 4, GAME_COUNT };

  ButtonNavigator buttonNavigator_;
  int selectedIndex_ = 0;

  void launchSelected();
};
