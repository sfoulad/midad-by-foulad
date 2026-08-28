#include "ChessGameActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "ChessPersona.h"
#include "ChessStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

uint32_t hookNowMs() { return millis(); }

uint32_t hookRandBelow(uint32_t bound) {
  return bound == 0 ? 0 : static_cast<uint32_t>(random(static_cast<long>(bound)));
}

// The search calls this every few thousand nodes. One tick is enough for the
// idle task to run and feed the watchdog without meaningfully slowing search.
void hookYield() { vTaskDelay(1); }

int outcomeFor(chess::Game::Status status, bool playerToMoveAtEnd) {
  switch (status) {
    case chess::Game::Status::Checkmate:
      // The side to move is the one that got mated.
      return playerToMoveAtEnd ? -1 : 1;
    case chess::Game::Status::Resigned:
      return -1;
    default:
      return 0;
  }
}

}  // namespace

void ChessGameActivity::onEnter() {
  Activity::onEnter();

  game_ = makeUniqueNoThrow<chess::Game>();
  search_ = makeUniqueNoThrow<chess::Search>();
  if (!game_ || !search_) {
    LOG_ERR("CHESS", "OOM: game %d B, search %d B", static_cast<int>(sizeof(chess::Game)),
            static_cast<int>(sizeof(chess::Search)));
    finish();
    return;
  }

  CHESS_STORE.loadFromFile();
  if (resume_ && CHESS_STORE.hasSavedGame()) {
    if (!game_->fromSaveString(CHESS_STORE.getSavedGame().c_str())) {
      LOG_ERR("CHESS", "Saved game did not replay; starting a new one");
      game_->reset();
    }
  }

  flipped_ = !playerIsWhite_;
  cursor_ = chess::squareOf(4, playerIsWhite_ ? 1 : 6);
  selected_ = -1;
  state_ = State::Playing;
  outcomeReported_ = false;
  // Back is often still held from the screen that launched this one; ignore
  // its release so the game does not exit the instant it opens.
  backHandled_ = mappedInput.isPressed(MappedInputManager::Button::Back);

  if (game_->plyCount() > 0) {
    const chess::Move& last = game_->moveAt(game_->plyCount() - 1);
    lastFrom_ = last.from;
    lastTo_ = last.to;
  }

  finishGameIfOver();
  if (state_ == State::Playing && !playerToMove()) beginEngineTurn();
  requestUpdate();
}

void ChessGameActivity::onExit() {
  stopEngineTask();
  search_.reset();
  game_.reset();
  Activity::onExit();
}

bool ChessGameActivity::playerToMove() const { return game_->board().whiteToMove() == playerIsWhite_; }

void ChessGameActivity::engineTaskTrampoline(void* context) { static_cast<ChessGameActivity*>(context)->runEngine(); }

void ChessGameActivity::runEngine() {
  chess::Search::Hooks hooks;
  hooks.nowMs = &hookNowMs;
  hooks.randBelow = &hookRandBelow;
  hooks.yieldCpu = &hookYield;
  engineMove_ = search_->findBestMove(searchBoard_, level_, hooks);
  engineFinished_ = true;
  // Signals onExit() that nothing touches this object any more.
  xSemaphoreGive(engineDoneSem_);
  vTaskDelete(nullptr);
}

bool ChessGameActivity::startEngineTask() {
  engineFinished_ = false;
  engineDoneSem_ = xSemaphoreCreateBinary();
  if (engineDoneSem_ == nullptr) {
    LOG_ERR("CHESS", "OOM creating engine semaphore");
    return false;
  }
  if (xTaskCreate(&engineTaskTrampoline, "chess-engine", ENGINE_STACK_BYTES, this, 1, &engineTask_) != pdPASS) {
    LOG_ERR("CHESS", "OOM creating engine task (%u B stack)", static_cast<unsigned>(ENGINE_STACK_BYTES));
    vSemaphoreDelete(engineDoneSem_);
    engineDoneSem_ = nullptr;
    return false;
  }
  return true;
}

void ChessGameActivity::stopEngineTask() {
  if (engineDoneSem_ == nullptr) return;
  // The task writes through `this`, so the activity must not be destroyed
  // while it lives. Abort makes the search unwind at its next node check.
  if (search_) search_->requestAbort();
  while (xSemaphoreTake(engineDoneSem_, pdMS_TO_TICKS(1000)) != pdTRUE) {
    LOG_ERR("CHESS", "engine task still running; waiting");
  }
  vSemaphoreDelete(engineDoneSem_);
  engineDoneSem_ = nullptr;
  engineTask_ = nullptr;
}

void ChessGameActivity::beginEngineTurn() {
  searchBoard_ = game_->board();
  if (!startEngineTask()) {
    // Without a task there is no game to play; fall back to ending the screen
    // rather than leaving the player stuck on a board that never replies.
    state_ = State::GameOver;
    outcome_ = 0;
    return;
  }
  state_ = State::Thinking;
  selected_ = -1;
  refreshLegalTargets();
}

void ChessGameActivity::refreshLegalTargets() {
  memset(legalTargets_, 0, sizeof(legalTargets_));
  if (selected_ < 0) return;
  chess::Move moves[chess::MAX_MOVES];
  const int count = game_->board().generateLegalMoves(moves, chess::MAX_MOVES);
  for (int i = 0; i < count; i++) {
    if (moves[i].from != selected_) continue;
    legalTargets_[chess::rankOf(moves[i].to) * 8 + chess::fileOf(moves[i].to)] = 1;
  }
}

void ChessGameActivity::moveCursor(int fileDelta, int rankDelta) {
  // The cursor moves in SCREEN directions, so flip the deltas when the board is
  // drawn from Black's side or "right" would walk left.
  const int step = flipped_ ? -1 : 1;
  int file = chess::fileOf(cursor_) + fileDelta * step;
  int rank = chess::rankOf(cursor_) + rankDelta * step;
  file = std::clamp(file, 0, 7);
  rank = std::clamp(rank, 0, 7);
  cursor_ = chess::squareOf(file, rank);
  requestUpdate();
}

void ChessGameActivity::applyPlayerMove(uint8_t from, uint8_t to) {
  chess::Move moves[chess::MAX_MOVES];
  const int count = game_->board().generateLegalMoves(moves, chess::MAX_MOVES);
  const chess::Move* chosen = nullptr;
  for (int i = 0; i < count; i++) {
    if (moves[i].from != from || moves[i].to != to) continue;
    // Auto-queen: the only promotion a player wants often enough to justify a
    // picker on four buttons.
    if ((moves[i].flags & chess::FLAG_PROMOTION) && moves[i].promo != chess::QUEEN) continue;
    chosen = &moves[i];
    break;
  }
  if (chosen == nullptr) return;

  lastFrom_ = chosen->from;
  lastTo_ = chosen->to;
  game_->play(*chosen);
  selected_ = -1;
  refreshLegalTargets();

  finishGameIfOver();
  if (state_ == State::Playing) {
    // No persist() here: the save happens once the engine has replied, so a
    // full move costs one SD write rather than two.
    beginEngineTurn();
  } else {
    persist();
  }
  requestUpdate();
}

void ChessGameActivity::handleConfirm() {
  if (state_ != State::Playing || !playerToMove()) return;

  if (selected_ >= 0) {
    if (selected_ == cursor_) {
      selected_ = -1;
      refreshLegalTargets();
      requestUpdate();
      return;
    }
    if (legalTargets_[chess::rankOf(cursor_) * 8 + chess::fileOf(cursor_)] != 0) {
      applyPlayerMove(static_cast<uint8_t>(selected_), static_cast<uint8_t>(cursor_));
      return;
    }
  }

  const chess::Piece piece = game_->board().at(static_cast<uint8_t>(cursor_));
  if (piece != chess::NO_PIECE && chess::isWhite(piece) == playerIsWhite_) {
    selected_ = cursor_;
    refreshLegalTargets();
    requestUpdate();
  }
}

void ChessGameActivity::finishGameIfOver() {
  const chess::Game::Status status = game_->status();
  if (status <= chess::Game::Status::Check) return;
  outcome_ = outcomeFor(status, playerToMove());
  state_ = State::GameOver;
  overlayIndex_ = 0;
  selected_ = -1;
  reportOutcome();
}

void ChessGameActivity::reportOutcome() {
  if (outcomeReported_) return;
  outcomeReported_ = true;
  // Clears the saved game too: a finished game is not resumable.
  CHESS_STORE.reportResult(static_cast<uint8_t>(level_), outcome_);
}

void ChessGameActivity::persist() {
  if (state_ == State::GameOver) return;
  char serialized[1200];
  if (!game_->toSaveString(serialized, sizeof(serialized))) return;
  CHESS_STORE.saveGame(serialized, static_cast<uint8_t>(level_), playerIsWhite_);
}

void ChessGameActivity::loop() {
  if (state_ == State::Thinking && engineFinished_) {
    stopEngineTask();
    if (!engineMove_.isNull()) {
      lastFrom_ = engineMove_.from;
      lastTo_ = engineMove_.to;
      game_->play(engineMove_);
    }
    state_ = State::Playing;
    finishGameIfOver();
    persist();
    requestUpdate();
    return;
  }

  if (state_ == State::GameOver || state_ == State::QuitMenu) {
    const int optionCount = (state_ == State::GameOver) ? OVER_OPTION_COUNT : QUIT_OPTION_COUNT;
    buttonNavigator_.onScrollNextRelease([this, optionCount] {
      overlayIndex_ = ButtonNavigator::nextIndex(overlayIndex_, optionCount);
      requestUpdate();
    });
    buttonNavigator_.onScrollPreviousRelease([this, optionCount] {
      overlayIndex_ = ButtonNavigator::previousIndex(overlayIndex_, optionCount);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (state_ == State::GameOver) {
        switch (overlayIndex_) {
          case 0:  // play again
            game_->reset();
            outcomeReported_ = false;
            lastFrom_ = lastTo_ = -1;
            state_ = State::Playing;
            cursor_ = chess::squareOf(4, playerIsWhite_ ? 1 : 6);
            if (!playerToMove()) beginEngineTurn();
            break;
          case 1:  // review the final position
            state_ = State::Playing;
            break;
          default:
            finish();
            return;
        }
      } else {
        switch (overlayIndex_) {
          case 0:  // resume
            state_ = State::Playing;
            break;
          case 1:  // save and exit
            persist();
            finish();
            return;
          default:  // resign
            game_->resign();
            finishGameIfOver();
            break;
        }
      }
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (state_ == State::QuitMenu) {
        state_ = State::Playing;
        requestUpdate();
      } else {
        finish();
      }
      return;
    }
    return;
  }

  // --- playing ---
  buttonNavigator_.onPress({MappedInputManager::Button::Up}, [this] { moveCursor(0, 1); });
  buttonNavigator_.onPress({MappedInputManager::Button::Down}, [this] { moveCursor(0, -1); });
  buttonNavigator_.onPress({MappedInputManager::Button::Left}, [this] { moveCursor(-1, 0); });
  buttonNavigator_.onPress({MappedInputManager::Button::Right}, [this] { moveCursor(1, 0); });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) handleConfirm();

  if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
    flipped_ = !flipped_;
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::PageBack) && state_ == State::Playing && playerToMove()) {
    // Take back the pair: the engine's reply and the player's own move.
    if (game_->undoPly()) game_->undoPly();
    selected_ = -1;
    lastFrom_ = lastTo_ = -1;
    if (game_->plyCount() > 0) {
      const chess::Move& last = game_->moveAt(game_->plyCount() - 1);
      lastFrom_ = last.from;
      lastTo_ = last.to;
    }
    refreshLegalTargets();
    persist();
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backHandled_ = false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (backHandled_) {
      backHandled_ = false;
      return;
    }
    if (mappedInput.getHeldTime() >= LONG_PRESS_MS) {
      state_ = State::QuitMenu;
      overlayIndex_ = 0;
    } else if (selected_ >= 0) {
      selected_ = -1;
      refreshLegalTargets();
    } else {
      persist();
      finish();
      return;
    }
    requestUpdate();
  }
}

void ChessGameActivity::drawPlayerBar(int y, bool opponentRow) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int left = layout_.x;
  const int right = layout_.x + layout_.size;
  const int nameHeight = renderer.getLineHeight(UI_12_FONT_ID);

  char name[48];
  char subtitle[32];
  if (opponentRow) {
    const auto& persona = chess_persona::at(level_);
    snprintf(name, sizeof(name), "%s", I18N.get(persona.name));
    snprintf(subtitle, sizeof(subtitle), "~%u", persona.rating);
  } else {
    snprintf(name, sizeof(name), "%s", tr(STR_CHESS_YOU));
    snprintf(subtitle, sizeof(subtitle), "%s", playerIsWhite_ ? tr(STR_CHESS_WHITE) : tr(STR_CHESS_BLACK));
  }

  renderer.drawText(UI_12_FONT_ID, left, y, name, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, left + renderer.getTextWidth(UI_12_FONT_ID, name, EpdFontFamily::BOLD) + 8,
                    y + (nameHeight - renderer.getLineHeight(UI_10_FONT_ID)), subtitle, true);

  // Captured pieces sit on the capturer's own row, so each side sees what it
  // has taken, next to its own name.
  const bool capturerIsWhite = opponentRow ? !playerIsWhite_ : playerIsWhite_;
  int count = 0;
  const chess::Piece* captured = game_->capturedBy(capturerIsWhite, count);
  const int lead = game_->materialLead(capturerIsWhite);

  char leadText[8] = {};
  if (lead > 0) snprintf(leadText, sizeof(leadText), "+%d", lead);
  const int leadWidth = leadText[0] == '\0' ? 0 : renderer.getTextWidth(UI_10_FONT_ID, leadText, EpdFontFamily::BOLD);

  const int glyph = chess_view::SMALL_GLYPH_SIZE;
  int x = right - leadWidth - (leadWidth > 0 ? 6 : 0) - count * (glyph - 4);
  x = std::max(x, left + pageWidth / 3);
  for (int i = 0; i < count; i++) {
    chess_view::drawPiece(renderer, captured[i], x, y, glyph);
    x += glyph - 4;  // slight overlap keeps a full set inside the row
  }
  if (leadText[0] != '\0') {
    renderer.drawText(UI_10_FONT_ID, right - leadWidth, y + (nameHeight - renderer.getLineHeight(UI_10_FONT_ID)),
                      leadText, true, EpdFontFamily::BOLD);
  }
  (void)metrics;
}

void ChessGameActivity::drawMoveList(int y) const {
  const int left = layout_.x;
  const int right = layout_.x + layout_.size;
  renderer.fillRect(left, y, right - left, 1, true);
  y += 8;

  // Last three move pairs, oldest at the top -- enough to see the current
  // sequence without turning the panel into a scoresheet.
  const int plies = game_->plyCount();
  const int pairs = (plies + 1) / 2;
  const int firstPair = std::max(0, pairs - 3);
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  for (int pair = firstPair; pair < pairs; pair++) {
    char number[8];
    snprintf(number, sizeof(number), "%d.", pair + 1);
    const int numberWidth = renderer.getTextWidth(UI_12_FONT_ID, number);
    renderer.drawText(UI_12_FONT_ID, left + 32 - numberWidth, y, number, true);
    renderer.drawText(UI_12_FONT_ID, left + 42, y, game_->sanAt(pair * 2), true);
    if (pair * 2 + 1 < plies) renderer.drawText(UI_12_FONT_ID, left + 132, y, game_->sanAt(pair * 2 + 1), true);
    y += lineHeight;
  }

  y = std::max(y, layout_.y + layout_.size + 4);
  const char* status = tr(STR_CHESS_YOUR_MOVE);
  if (state_ == State::Thinking) {
    status = tr(STR_CHESS_THINKING);
  } else if (state_ == State::GameOver) {
    status = tr(STR_CHESS_GAME_OVER);
  } else if (game_->board().inCheck(game_->board().whiteToMove())) {
    status = tr(STR_CHESS_CHECK);
  } else if (!playerToMove()) {
    status = tr(STR_CHESS_THINKING);
  }
  renderer.drawText(UI_12_FONT_ID, left, y + 6, status, true, EpdFontFamily::BOLD);
}

void ChessGameActivity::drawOverlay() const {
  if (state_ != State::GameOver && state_ != State::QuitMenu) return;

  const int pageWidth = renderer.getScreenWidth();
  const int boxWidth = std::min(360, layout_.size);
  const int boxX = layout_.x + (layout_.size - boxWidth) / 2;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int rowHeight = UITheme::getInstance().getMetrics().listRowHeight;

  char title[64];
  char detail[80];
  char record[64];
  int optionCount;
  const char* options[3];

  if (state_ == State::GameOver) {
    const auto& persona = chess_persona::at(level_);
    if (outcome_ > 0) {
      snprintf(title, sizeof(title), "%s", tr(STR_CHESS_YOU_WIN));
      snprintf(detail, sizeof(detail), tr(STR_CHESS_BEAT_FMT), I18N.get(persona.name), persona.rating,
               (game_->plyCount() + 1) / 2);
    } else if (outcome_ < 0) {
      snprintf(title, sizeof(title), "%s", tr(STR_CHESS_YOU_LOSE));
      snprintf(detail, sizeof(detail), tr(STR_CHESS_LOST_TO_FMT), I18N.get(persona.name), persona.rating);
    } else {
      snprintf(title, sizeof(title), "%s", tr(STR_CHESS_DRAW));
      snprintf(detail, sizeof(detail), tr(STR_CHESS_LOST_TO_FMT), I18N.get(persona.name), persona.rating);
    }
    snprintf(record, sizeof(record), tr(STR_CHESS_RECORD_VS_FMT), I18N.get(persona.name),
             CHESS_STORE.getWins(static_cast<uint8_t>(level_)), CHESS_STORE.getLosses(static_cast<uint8_t>(level_)),
             CHESS_STORE.getDraws(static_cast<uint8_t>(level_)));
    optionCount = OVER_OPTION_COUNT;
    options[0] = tr(STR_CHESS_PLAY_AGAIN);
    options[1] = tr(STR_CHESS_REVIEW);
    options[2] = tr(STR_CHESS_BACK_TO_MENU);
  } else {
    snprintf(title, sizeof(title), "%s", tr(STR_CHESS_PAUSED));
    detail[0] = '\0';
    record[0] = '\0';
    optionCount = QUIT_OPTION_COUNT;
    options[0] = tr(STR_CHESS_RESUME);
    options[1] = tr(STR_CHESS_SAVE_EXIT);
    options[2] = tr(STR_CHESS_RESIGN);
  }

  const int detailLines = (detail[0] != '\0' ? 1 : 0) + (record[0] != '\0' ? 1 : 0);
  const int boxHeight = 20 + lineHeight * 2 + detailLines * (lineHeight + 2) + 12 + rowHeight * optionCount + 20;
  const int boxY = layout_.y + (layout_.size - boxHeight) / 2;

  renderer.fillRect(boxX, boxY, boxWidth, boxHeight, false);
  renderer.drawRect(boxX, boxY, boxWidth, boxHeight, 3, true);

  int y = boxY + 16;
  UITheme::drawCenteredText(renderer, Rect{boxX, boxY, boxWidth, boxHeight}, UI_12_FONT_ID, y, title, true,
                            EpdFontFamily::BOLD);
  y += lineHeight + 6;
  if (detail[0] != '\0') {
    UITheme::drawCenteredText(renderer, Rect{boxX, boxY, boxWidth, boxHeight}, UI_10_FONT_ID, y, detail);
    y += lineHeight;
  }
  if (record[0] != '\0') {
    UITheme::drawCenteredText(renderer, Rect{boxX, boxY, boxWidth, boxHeight}, UI_10_FONT_ID, y, record);
    y += lineHeight;
  }

  y += 6;
  renderer.fillRect(boxX + 16, y, boxWidth - 32, 1, true);
  y += 6;

  for (int i = 0; i < optionCount; i++) {
    const bool selected = (i == overlayIndex_);
    if (selected) renderer.fillRect(boxX + 8, y - 2, boxWidth - 16, rowHeight, true);
    renderer.drawText(UI_12_FONT_ID, boxX + 20, y + (rowHeight - lineHeight) / 2, options[i], !selected);
    y += rowHeight;
  }
  (void)pageWidth;
}

void ChessGameActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CHESS));

  const int barHeight = renderer.getLineHeight(UI_12_FONT_ID) + 6;
  const int topBarY = metrics.topPadding + metrics.headerHeight + 4;
  const int boardTop = topBarY + barHeight;
  const int hintsTop = pageHeight - metrics.buttonHintsHeight;
  // The board takes what is left after both player bars, the move list and the
  // hint bar; computeLayout() then squares it off.
  const int reservedBelow = barHeight + renderer.getLineHeight(UI_12_FONT_ID) * 4 + 20;
  layout_ = chess_view::computeLayout(
      renderer, Rect{metrics.contentSidePadding, boardTop, pageWidth - metrics.contentSidePadding * 2,
                     hintsTop - boardTop - reservedBelow});

  drawPlayerBar(topBarY, /*opponentRow=*/true);

  chess_view::Highlights highlights;
  highlights.cursor = (state_ == State::Playing && playerToMove()) ? cursor_ : -1;
  highlights.selected = selected_;
  highlights.lastFrom = lastFrom_;
  highlights.lastTo = lastTo_;
  highlights.legalTargets = (selected_ >= 0) ? legalTargets_ : nullptr;
  chess_view::drawBoard(renderer, game_->board(), layout_, highlights, flipped_);

  const int bottomBarY = layout_.y + layout_.size + 6;
  drawPlayerBar(bottomBarY, /*opponentRow=*/false);
  drawMoveList(bottomBarY + barHeight + 2);

  drawOverlay();

  const char* backLabel = (selected_ >= 0) ? tr(STR_CANCEL) : tr(STR_BACK);
  const char* confirmLabel = (selected_ >= 0) ? tr(STR_CHESS_MOVE) : tr(STR_SELECT);
  if (state_ == State::GameOver || state_ == State::QuitMenu) {
    backLabel = tr(STR_BACK);
    confirmLabel = tr(STR_SELECT);
  }
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT),
                                            /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Periodic HALF refresh: FAST leaves residue, and a dithered board shows it.
  if (++rendersSinceFullRefresh_ >= 20) {
    rendersSinceFullRefresh_ = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer();
  }
}
