#include "GamesMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "MazeActivity.h"
#include "SnakeActivity.h"
#include "SudokuActivity.h"
#include "TetrisActivity.h"
#include "components/UITheme.h"

void GamesMenuActivity::onEnter() {
  Activity::onEnter();
  selectedIndex_ = 0;
  requestUpdate();
}

void GamesMenuActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Logical NavNext/NavPrevious (not raw Up/Down) -- resolves to whichever
  // physical buttons are correct for this device/orientation, same as every
  // other list in the app (see ButtonNavigator::getNextButtons/getPreviousButtons).
  buttonNavigator_.onNextRelease([this] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, GAME_COUNT);
    requestUpdate();
  });
  buttonNavigator_.onPreviousRelease([this] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, GAME_COUNT);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    launchSelected();
  }
}

void GamesMenuActivity::launchSelected() {
  // No-op result handler: startActivityForResult() already triggers a
  // requestUpdate() once the game finishes, so we just need to be re-shown.
  switch (selectedIndex_) {
    case SNAKE:
      startActivityForResult(std::make_unique<SnakeActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      break;
    case TETRIS:
      startActivityForResult(std::make_unique<TetrisActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      break;
    case SUDOKU:
      startActivityForResult(std::make_unique<SudokuActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      break;
    case MAZE:
    default:
      startActivityForResult(std::make_unique<MazeActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      break;
  }
}

void GamesMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GAMES));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listHeight}, GAME_COUNT, selectedIndex_, [](int index) {
    switch (index) {
      case SNAKE:
        return tr(STR_SNAKE);
      case TETRIS:
        return tr(STR_TETRIS);
      case SUDOKU:
        return tr(STR_SUDOKU);
      default:
        return tr(STR_MAZE);
    }
  });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
