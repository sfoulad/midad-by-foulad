#include "ChessLevelActivity.h"

#include <I18n.h>

#include <cstdio>

#include "ChessPersona.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ChessLevelActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

const char* ChessLevelActivity::sideLabel(int index) const {
  static const StrId LABELS[SIDE_COUNT] = {StrId::STR_CHESS_WHITE, StrId::STR_CHESS_BLACK, StrId::STR_CHESS_RANDOM};
  return I18N.get(LABELS[index]);
}

void ChessLevelActivity::loop() {
  if (sideListOpen_) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      sideListOpen_ = false;
      requestUpdate();
      return;
    }
    buttonNavigator_.onScrollNextRelease([this] {
      sideListIndex_ = ButtonNavigator::nextIndex(sideListIndex_, SIDE_COUNT);
      requestUpdate();
    });
    buttonNavigator_.onScrollPreviousRelease([this] {
      sideListIndex_ = ButtonNavigator::previousIndex(sideListIndex_, SIDE_COUNT);
      requestUpdate();
    });
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      side_ = static_cast<Side>(sideListIndex_);
      sideListOpen_ = false;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    setResult(ActivityResult{});
    result.isCancelled = true;
    finish();
    return;
  }

  // The persona list and the side row share one vertical cursor: moving past
  // the last persona lands on the side row, which then opens its own list.
  buttonNavigator_.onScrollNextRelease([this] {
    if (sideRowFocused_) {
      sideRowFocused_ = false;
      selectedLevel_ = 0;
    } else if (selectedLevel_ + 1 >= chess::LEVEL_COUNT) {
      sideRowFocused_ = true;
    } else {
      selectedLevel_++;
    }
    requestUpdate();
  });
  buttonNavigator_.onScrollPreviousRelease([this] {
    if (sideRowFocused_) {
      sideRowFocused_ = false;
      selectedLevel_ = chess::LEVEL_COUNT - 1;
    } else if (selectedLevel_ == 0) {
      sideRowFocused_ = true;
    } else {
      selectedLevel_--;
    }
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (sideRowFocused_) {
      sideListIndex_ = static_cast<int>(side_);
      sideListOpen_ = true;
      requestUpdate();
      return;
    }
    confirmSelection();
  }
}

void ChessLevelActivity::confirmSelection() {
  ChessSetupResult choice;
  choice.level = selectedLevel_;
  choice.playerIsWhite = (side_ == Side::White) || (side_ == Side::Random && (random(2) == 0));
  setResult(ActivityResult{choice});
  finish();
}

void ChessLevelActivity::renderSideList() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CHESS_YOUR_SIDE));

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawList(renderer, Rect{0, listTop, pageWidth, metrics.listRowHeight * SIDE_COUNT}, SIDE_COUNT, sideListIndex_,
               [this](int index) -> std::string { return sideLabel(index); });

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN), /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ChessLevelActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (sideListOpen_) {
    renderSideList();
    renderer.displayBuffer();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int side = metrics.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CHESS_NEW_GAME));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, side, y, tr(STR_CHESS_OPPONENT), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 4;

  // Persona rows carry a rating and a one-line style hint, so they use the
  // subtitle row height rather than the plain one.
  const int rowHeight = metrics.listWithSubtitleRowHeight;
  // static because drawList's row callbacks are non-capturing and read it
  // back after this scope would have ended for a plain local.
  static char subtitles[chess::LEVEL_COUNT][80];
  for (int i = 0; i < chess::LEVEL_COUNT; i++) {
    snprintf(subtitles[i], sizeof(subtitles[i]), "~%u - %s", chess_persona::at(i).rating,
             I18N.get(chess_persona::at(i).styleHint));
  }

  GUI.drawList(
      renderer, Rect{0, y, pageWidth, rowHeight * chess::LEVEL_COUNT}, chess::LEVEL_COUNT,
      sideRowFocused_ ? -1 : selectedLevel_,
      [](int index) -> std::string { return I18N.get(chess_persona::at(index).name); },
      [](int index) -> std::string { return subtitles[index]; });

  // One-row list rather than hand-drawn text: the drill-down row reads as a row
  // you can enter, with the chosen side in the value column, exactly like every
  // other settings row in the firmware.
  y += rowHeight * chess::LEVEL_COUNT + metrics.verticalSpacing;
  GUI.drawList(
      renderer, Rect{0, y, pageWidth, metrics.listRowHeight}, 1, sideRowFocused_ ? 0 : -1,
      [](int) -> std::string { return tr(STR_CHESS_YOUR_SIDE); }, nullptr, nullptr,
      [this](int) -> std::string { return sideLabel(static_cast<int>(side_)); });

  UITheme::drawCenteredText(
      renderer, Rect{0, 0, pageWidth, pageHeight}, UI_10_FONT_ID,
      pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - renderer.getLineHeight(UI_10_FONT_ID),
      tr(STR_CHESS_RATINGS_APPROX));

  // Confirm means "open the side list" on the side row and "start" everywhere
  // else, so the hint has to say which.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), sideRowFocused_ ? tr(STR_SELECT) : tr(STR_CHESS_START),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN),
                                            /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
