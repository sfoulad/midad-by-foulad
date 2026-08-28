#include "ChessOpeningViewActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "OpeningBook.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Copies the `index`-th SAN token out of a space-separated move string.
bool sanTokenAt(const char* moves, int index, char* out, size_t cap) {
  const char* p = moves;
  for (int i = 0; i <= index; i++) {
    while (*p == ' ') p++;
    if (*p == '\0') return false;
    const char* start = p;
    while (*p != '\0' && *p != ' ') p++;
    if (i == index) {
      const size_t length = static_cast<size_t>(p - start);
      if (length + 1 > cap) return false;
      memcpy(out, start, length);
      out[length] = '\0';
      return true;
    }
  }
  return false;
}

int countPlies(const char* moves) {
  int count = 0;
  const char* p = moves;
  while (*p != '\0') {
    while (*p == ' ') p++;
    if (*p == '\0') break;
    count++;
    while (*p != '\0' && *p != ' ') p++;
  }
  return count;
}

}  // namespace

void ChessOpeningViewActivity::onEnter() {
  Activity::onEnter();
  game_ = makeUniqueNoThrow<chess::Game>();
  if (!game_) {
    LOG_ERR("CHESS", "OOM: opening walkthrough game %d B", static_cast<int>(sizeof(chess::Game)));
    finish();
    return;
  }
  totalPlies_ = countPlies(chess_book::lineAt(lineIndex_).moves);
  gotoPly(0);
  requestUpdate();
}

void ChessOpeningViewActivity::onExit() {
  game_.reset();
  Activity::onExit();
}

void ChessOpeningViewActivity::gotoPly(int ply) {
  ply_ = std::clamp(ply, 0, totalPlies_);
  const auto& line = chess_book::lineAt(lineIndex_);
  game_->reset();
  for (int i = 0; i < ply_; i++) {
    char san[12];
    if (!sanTokenAt(line.moves, i, san, sizeof(san)) || !game_->playSan(san)) {
      // Only reachable if the book itself is wrong; stop where it broke rather
      // than showing a position that does not match the move list.
      LOG_ERR("CHESS", "Opening %d: move %d did not parse", lineIndex_, i + 1);
      ply_ = i;
      totalPlies_ = i;
      break;
    }
  }
}

void ChessOpeningViewActivity::loop() {
  const bool hasTip = (chess_book::tipForPly(chess_book::lineAt(lineIndex_), ply_) != nullptr);

  if (focus_ != Focus::Board) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      focus_ = (focus_ == Focus::TipReading) ? Focus::Tip : Focus::Board;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      focus_ = (focus_ == Focus::Tip) ? Focus::TipReading : Focus::Board;
      requestUpdate();
      return;
    }
    // Up steps back onto the board from the highlighted block, matching the game
    // screen; inside the opened page it does nothing, Back closes it.
    buttonNavigator_.onPress({MappedInputManager::Button::Up}, [this] {
      if (focus_ != Focus::Tip) return;
      focus_ = Focus::Board;
      requestUpdate();
    });
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  buttonNavigator_.onPress({MappedInputManager::Button::Right}, [this] {
    if (ply_ >= totalPlies_) return;
    gotoPly(ply_ + 1);
    requestUpdate();
  });
  buttonNavigator_.onPress({MappedInputManager::Button::Left}, [this] {
    if (ply_ <= 0) return;
    gotoPly(ply_ - 1);
    requestUpdate();
  });

  // The side buttons carry the vertical axis, so they step onto the tip block
  // rather than duplicating Left/Right's ply stepping as they used to.
  buttonNavigator_.onPress({MappedInputManager::Button::Down}, [this, hasTip] {
    if (!hasTip) return;
    focus_ = Focus::Tip;
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    gotoPly(0);
    requestUpdate();
  }
}

void ChessOpeningViewActivity::drawMoveChips(int y, int width) const {
  const auto& line = chess_book::lineAt(lineIndex_);
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  int x = layout_.x;
  const int right = layout_.x + width;

  for (int i = 0; i < totalPlies_; i++) {
    char san[12];
    if (!sanTokenAt(line.moves, i, san, sizeof(san))) break;
    char chip[16];
    if (i % 2 == 0) {
      snprintf(chip, sizeof(chip), "%d.%s", i / 2 + 1, san);
    } else {
      snprintf(chip, sizeof(chip), "%s", san);
    }

    const int chipWidth = renderer.getTextWidth(SMALL_FONT_ID, chip) + 8;
    if (x + chipWidth > right) {
      x = layout_.x;
      y += lineHeight + 2;
    }
    const bool current = (i == ply_ - 1);
    if (current) renderer.fillRect(x - 2, y - 2, chipWidth, lineHeight + 4, true);
    renderer.drawText(SMALL_FONT_ID, x + 2, y, chip, !current);
    x += chipWidth;
  }
}

// The tip on its own page: no board above it, so the body gets the whole screen
// and the longer notes stop being cut off after four lines.
void ChessOpeningViewActivity::renderTipPage() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& line = chess_book::lineAt(lineIndex_);
  const chess_book::Tip* tip = chess_book::tipForPly(line, ply_);
  if (tip == nullptr) return;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int side = metrics.contentSidePadding;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, I18N.get(tip->title));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  char plyLabel[48];
  snprintf(plyLabel, sizeof(plyLabel), tr(STR_CHESS_MOVE_FMT), (ply_ + 1) / 2,
           (ply_ % 2 == 1) ? tr(STR_CHESS_WHITE) : tr(STR_CHESS_BLACK));
  renderer.drawText(UI_10_FONT_ID, side, y, plyLabel, true);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 6;
  renderer.fillRect(side, y, pageWidth - side * 2, 1, true);
  y += 10;

  const int bodyHeight = hintsTop - y - metrics.verticalSpacing;
  UITheme::drawCenteredWrappedText(renderer, Rect{side, y, pageWidth - side * 2, bodyHeight}, UI_12_FONT_ID,
                                   I18N.get(tip->body), /*maxLines=*/bodyHeight / renderer.getLineHeight(UI_12_FONT_ID),
                                   true, EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CHESS_BACK_TO_BOARD), "", "", /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ChessOpeningViewActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (focus_ == Focus::TipReading) {
    renderTipPage();
    renderer.displayBuffer();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& line = chess_book::lineAt(lineIndex_);
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int contentWidth = pageWidth - metrics.contentSidePadding * 2;

  char header[64];
  snprintf(header, sizeof(header), "%s - %s", I18N.get(line.name), line.eco);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header);

  const int chipsTop = metrics.topPadding + metrics.headerHeight + 4;
  const int chipRows = 2;
  const int chipsHeight = (renderer.getLineHeight(SMALL_FONT_ID) + 2) * chipRows;
  const int boardTop = chipsTop + chipsHeight + 4;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;
  // Room under the board for the ply line, the rule and a three-line tip.
  const int reservedBelow = renderer.getLineHeight(UI_12_FONT_ID) * 5 + 24;

  layout_ = chess_view::computeLayout(
      renderer, Rect{metrics.contentSidePadding, boardTop, contentWidth, hintsTop - boardTop - reservedBelow});

  drawMoveChips(chipsTop, contentWidth);

  chess_view::Highlights highlights;
  if (ply_ > 0) {
    const chess::Move& last = game_->moveAt(game_->plyCount() - 1);
    highlights.lastFrom = last.from;
    highlights.lastTo = last.to;
  }
  chess_view::drawBoard(renderer, game_->board(), layout_, highlights, /*flipped=*/false);

  int y = layout_.y + layout_.size + 8;
  char plyLabel[48];
  if (ply_ == 0) {
    snprintf(plyLabel, sizeof(plyLabel), "%s", tr(STR_CHESS_START_POSITION));
  } else {
    snprintf(plyLabel, sizeof(plyLabel), tr(STR_CHESS_MOVE_FMT), (ply_ + 1) / 2,
             (ply_ % 2 == 1) ? tr(STR_CHESS_WHITE) : tr(STR_CHESS_BLACK));
  }
  const char* sideLabel = (ply_ >= totalPlies_) ? tr(STR_CHESS_END_OF_LINE)
                          : (ply_ % 2 == 0)     ? tr(STR_CHESS_WHITE_TO_MOVE)
                                                : tr(STR_CHESS_BLACK_TO_MOVE);
  renderer.drawText(UI_10_FONT_ID, layout_.x, y, plyLabel, true);
  renderer.drawText(UI_10_FONT_ID, layout_.x + layout_.size - renderer.getTextWidth(UI_10_FONT_ID, sideLabel), y,
                    sideLabel, true);

  y += renderer.getLineHeight(UI_10_FONT_ID) + 4;
  renderer.fillRect(layout_.x, y, layout_.size, 1, true);
  y += 8;

  const chess_book::Tip* tip = chess_book::tipForPly(line, ply_);
  if (tip != nullptr) {
    const int blockTop = y;
    renderer.drawText(UI_10_FONT_ID, layout_.x, y, I18N.get(tip->title), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 2;
    UITheme::drawCenteredWrappedText(renderer, Rect{layout_.x, y, layout_.size, hintsTop - y - 4}, UI_12_FONT_ID,
                                     I18N.get(tip->body), /*maxLines=*/4, true, EpdFontFamily::REGULAR,
                                     UITheme::TextVerticalAlignment::TOP);
    if (focus_ == Focus::Tip) {
      renderer.drawRect(layout_.x - 4, blockTop - 4, layout_.size + 8, hintsTop - blockTop, 2, true);
    } else {
      // Only advertised when there is something to open, and only from the board.
      const char* hint = tr(STR_CHESS_HINT_READ_MORE);
      renderer.drawText(SMALL_FONT_ID, layout_.x + layout_.size - renderer.getTextWidth(SMALL_FONT_ID, hint), blockTop,
                        hint, true);
    }
  } else {
    renderer.drawText(UI_10_FONT_ID, layout_.x, y, tr(STR_CHESS_BOOK_MOVE), true);
  }

  const auto labels =
      (focus_ == Focus::Tip)
          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_CHESS_READ_MORE), "", "", /*rtlSwap=*/false)
          : mappedInput.mapLabels(tr(STR_BACK), tr(STR_CHESS_RESTART), tr(STR_CHESS_PREV), tr(STR_CHESS_NEXT),
                                  /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
