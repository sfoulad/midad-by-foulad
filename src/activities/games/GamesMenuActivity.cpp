#include "GamesMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "SnakeActivity.h"
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
  if (selectedIndex_ == SNAKE) {
    startActivityForResult(std::make_unique<SnakeActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  } else {
    startActivityForResult(std::make_unique<TetrisActivity>(renderer, mappedInput), [](const ActivityResult&) {});
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

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, GAME_COUNT, selectedIndex_,
      [](int index) { return index == SNAKE ? tr(STR_SNAKE) : tr(STR_TETRIS); });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
