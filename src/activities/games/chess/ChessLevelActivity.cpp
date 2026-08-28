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

void ChessLevelActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    setResult(ActivityResult{});
    result.isCancelled = true;
    finish();
    return;
  }

  // The persona list and the side row share one vertical cursor: moving past
  // the last persona lands on the side row, which then takes Left/Right.
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

  buttonNavigator_.onPress({MappedInputManager::Button::Right}, [this] {
    if (!sideRowFocused_) return;
    side_ = static_cast<Side>((static_cast<uint8_t>(side_) + 1) % 3);
    requestUpdate();
  });
  buttonNavigator_.onPress({MappedInputManager::Button::Left}, [this] {
    if (!sideRowFocused_) return;
    side_ = static_cast<Side>((static_cast<uint8_t>(side_) + 2) % 3);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmSelection();
}

void ChessLevelActivity::confirmSelection() {
  ChessSetupResult choice;
  choice.level = selectedLevel_;
  choice.playerIsWhite = (side_ == Side::White) || (side_ == Side::Random && (random(2) == 0));
  setResult(ActivityResult{choice});
  finish();
}

void ChessLevelActivity::render(RenderLock&&) {
  renderer.clearScreen();

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

  y += rowHeight * chess::LEVEL_COUNT + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, side, y, tr(STR_CHESS_YOUR_SIDE), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 4;

  const StrId sideLabels[3] = {StrId::STR_CHESS_WHITE, StrId::STR_CHESS_BLACK, StrId::STR_CHESS_RANDOM};
  if (sideRowFocused_) renderer.fillRect(0, y - 2, pageWidth, metrics.listRowHeight, true);
  int x = side;
  for (int i = 0; i < 3; i++) {
    const bool chosen = (static_cast<int>(side_) == i);
    const char* label = I18N.get(sideLabels[i]);
    const EpdFontFamily::Style style = chosen ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    if (chosen) {
      renderer.drawText(UI_12_FONT_ID, x, y, "<", !sideRowFocused_);
      x += renderer.getTextWidth(UI_12_FONT_ID, "< ");
    }
    renderer.drawText(UI_12_FONT_ID, x, y, label, !sideRowFocused_, style);
    x += renderer.getTextWidth(UI_12_FONT_ID, label, style);
    if (chosen) {
      renderer.drawText(UI_12_FONT_ID, x + 4, y, ">", !sideRowFocused_);
      x += renderer.getTextWidth(UI_12_FONT_ID, " >");
    }
    x += renderer.getTextWidth(UI_12_FONT_ID, "   ");
  }

  UITheme::drawCenteredText(
      renderer, Rect{0, 0, pageWidth, pageHeight}, UI_10_FONT_ID,
      pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - renderer.getLineHeight(UI_10_FONT_ID),
      tr(STR_CHESS_RATINGS_APPROX));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CHESS_START), tr(STR_DIR_UP), tr(STR_DIR_DOWN),
                                            /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
