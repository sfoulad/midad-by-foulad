#include "ChessMenuActivity.h"

#include <I18n.h>

#include <cstdio>

#include "ChessGameActivity.h"
#include "ChessLevelActivity.h"
#include "ChessOpeningsActivity.h"
#include "ChessPersona.h"
#include "ChessStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ChessMenuActivity::onEnter() {
  Activity::onEnter();
  CHESS_STORE.loadFromFile();
  hasSaved_ = CHESS_STORE.hasSavedGame();
  if (selectedIndex_ >= rowCount()) selectedIndex_ = 0;
  requestUpdate();
}

ChessMenuActivity::Row ChessMenuActivity::rowKindAt(int index) const {
  if (index == 0) return Row::NewGame;
  if (hasSaved_ && index == 1) return Row::Continue;
  return Row::Openings;
}

void ChessMenuActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  buttonNavigator_.onScrollNextRelease([this] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, rowCount());
    requestUpdate();
  });
  buttonNavigator_.onScrollPreviousRelease([this] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, rowCount());
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) launchSelected();
}

void ChessMenuActivity::startGame(int level, bool playerIsWhite, bool resume) {
  startActivityForResult(std::make_unique<ChessGameActivity>(renderer, mappedInput, level, playerIsWhite, resume),
                         [this](const ActivityResult&) { hasSaved_ = CHESS_STORE.hasSavedGame(); });
}

void ChessMenuActivity::launchSelected() {
  switch (rowKindAt(selectedIndex_)) {
    case Row::NewGame:
      startActivityForResult(std::make_unique<ChessLevelActivity>(renderer, mappedInput),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) return;
                               const auto* choice = std::get_if<ChessSetupResult>(&result.data);
                               if (choice == nullptr) return;
                               startGame(choice->level, choice->playerIsWhite, /*resume=*/false);
                             });
      break;
    case Row::Continue:
      startGame(CHESS_STORE.getSavedLevel(), CHESS_STORE.getSavedPlayerIsWhite(), /*resume=*/true);
      break;
    case Row::Openings:
      startActivityForResult(std::make_unique<ChessOpeningsActivity>(renderer, mappedInput),
                             [](const ActivityResult&) {});
      break;
  }
}

void ChessMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CHESS));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int rows = rowCount();
  const int listHeight = rows * metrics.listRowHeight;

  // Built once per render: drawList's row callbacks are called repeatedly, and
  // formatting inside them would rebuild the same string on every pass.
  char continueLabel[64] = {};
  if (hasSaved_) {
    const int level = chess_persona::clampLevel(CHESS_STORE.getSavedLevel());
    snprintf(continueLabel, sizeof(continueLabel), "%s %s", tr(STR_CHESS_CONTINUE_VS),
             I18N.get(chess_persona::at(level).name));
  }

  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listHeight}, rows, selectedIndex_,
               [this, &continueLabel](int index) -> std::string {
                 switch (rowKindAt(index)) {
                   case Row::NewGame:
                     return tr(STR_CHESS_NEW_GAME);
                   case Row::Continue:
                     return continueLabel;
                   default:
                     return tr(STR_CHESS_LEARN_OPENINGS);
                 }
               });

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN), /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
