#include "ChessOpeningsActivity.h"

#include <I18n.h>

#include <cstdio>

#include "ChessOpeningViewActivity.h"
#include "MappedInputManager.h"
#include "OpeningBook.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ChessOpeningsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ChessOpeningsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  buttonNavigator_.onScrollNextRelease([this] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, chess_book::lineCount());
    requestUpdate();
  });
  buttonNavigator_.onScrollPreviousRelease([this] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, chess_book::lineCount());
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    startActivityForResult(std::make_unique<ChessOpeningViewActivity>(renderer, mappedInput, selectedIndex_),
                           [](const ActivityResult&) {});
  }
}

void ChessOpeningsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CHESS_LEARN_OPENINGS));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listHeight}, chess_book::lineCount(), selectedIndex_,
      [](int index) -> std::string { return I18N.get(chess_book::lineAt(index).name); },
      [](int index) -> std::string {
        // "C50 - 1.e4 e5 2.Nf3 Nc6": enough of the line to recognise it
        // without opening the walkthrough.
        const auto& line = chess_book::lineAt(index);
        char preview[64];
        chess_book::formatPreview(line, 5, preview, sizeof(preview));
        char subtitle[80];
        snprintf(subtitle, sizeof(subtitle), "%s - %s", line.eco, preview);
        return subtitle;
      });

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN), /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
