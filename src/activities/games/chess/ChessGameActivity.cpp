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
  const int file = std::clamp(chess::fileOf(cursor_) + fileDelta * step, 0, 7);
  const int rank = chess::rankOf(cursor_) + rankDelta * step;

  // Pressing Down on the bottom rank leaves the board for the move list rather
  // than doing nothing. Only downwards: the top rank still clamps, because there
  // is nothing above the board to step onto.
  if (rankDelta != 0 && (rank < 0 || rank > 7)) {
    const bool leavingBottom = flipped_ ? (rank > 7) : (rank < 0);
    if (leavingBottom && rankDelta < 0 && game_->plyCount() > 0) {
      enterRecord();
    }
    requestUpdate();
    return;
  }

  cursor_ = chess::squareOf(file, std::clamp(rank, 0, 7));
  requestUpdate();
}

void ChessGameActivity::cursorToOwnKing() {
  const chess::Piece king = chess::makePiece(chess::KING, playerIsWhite_);
  for (int square = 0; square < 128; square++) {
    if ((square & 0x88) != 0) continue;
    if (game_->board().at(static_cast<uint8_t>(square)) == king) {
      cursor_ = square;
      return;
    }
  }
}

void ChessGameActivity::enterRecord() {
  reviewPly_ = game_->plyCount();
  if (!game_->positionAt(reviewPly_, reviewBoard_)) return;
  focus_ = Focus::Record;
  selected_ = -1;
  refreshLegalTargets();
  requestUpdate();
}

void ChessGameActivity::leaveRecord() {
  focus_ = Focus::Board;
  cursorToOwnKing();
  requestUpdate();
}

const chess::Board& ChessGameActivity::visibleBoard() const {
  return focus_ == Focus::Record ? reviewBoard_ : game_->board();
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
  // Nothing to record -- no win/loss ledger, the game is meant to be fun rather
  // than scored. All a finished game leaves behind is that it is not resumable.
  CHESS_STORE.clearSavedGame();
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

  if (state_ == State::GameOver || state_ == State::QuitMenu || state_ == State::ResignConfirm) {
    const int optionCount = (state_ == State::GameOver)   ? OVER_OPTION_COUNT
                            : (state_ == State::QuitMenu) ? QUIT_OPTION_COUNT
                                                          : RESIGN_OPTION_COUNT;
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
      } else if (state_ == State::QuitMenu) {
        switch (overlayIndex_) {
          case 0:  // resume
            state_ = State::Playing;
            break;
          case 1:  // save and exit
            persist();
            finish();
            return;
          default:  // resign -- asks first, see State::ResignConfirm
            state_ = State::ResignConfirm;
            overlayIndex_ = 0;
            break;
        }
      } else {
        // Resigning is the one move in the game that cannot be taken back, and it
        // sits one button away from the record, so it asks.
        if (overlayIndex_ == 0) {
          state_ = State::Playing;
        } else {
          state_ = State::Playing;
          game_->resign();
          finishGameIfOver();
        }
      }
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (state_ == State::QuitMenu || state_ == State::ResignConfirm) {
        state_ = State::Playing;
        requestUpdate();
      } else {
        finish();
      }
      return;
    }
    return;
  }

  // --- the record ---
  //
  // Board, Resign, Prev, Next on the four front buttons, and the side buttons step
  // the record too: on the X3 they sit on the left and right edges of the panel, so
  // the left one is Prev and the right one is Next, matching the front pair rather
  // than inventing a second meaning for them.
  if (focus_ == Focus::Record) {
    buttonNavigator_.onPress({MappedInputManager::Button::Left, MappedInputManager::Button::Up}, [this] {
      if (reviewPly_ > 0) {
        reviewPly_--;
        game_->positionAt(reviewPly_, reviewBoard_);
        requestUpdate();
      }
    });
    buttonNavigator_.onPress({MappedInputManager::Button::Right, MappedInputManager::Button::Down}, [this] {
      if (reviewPly_ < game_->plyCount()) {
        reviewPly_++;
        game_->positionAt(reviewPly_, reviewBoard_);
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state_ = State::ResignConfirm;
      overlayIndex_ = 0;
      leaveRecord();
      return;
    }

    // Back on RELEASE, like the board branch below: acting on the press would
    // leave the release edge unconsumed, and the board branch would read it on the
    // very next frame and exit the game.
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (backHandled_) {
        backHandled_ = false;
        return;
      }
      leaveRecord();
    }
    return;
  }

  // --- playing ---
  buttonNavigator_.onPress({MappedInputManager::Button::Up}, [this] { moveCursor(0, 1); });
  buttonNavigator_.onPress({MappedInputManager::Button::Down}, [this] { moveCursor(0, -1); });
  buttonNavigator_.onPress({MappedInputManager::Button::Left}, [this] { moveCursor(-1, 0); });
  buttonNavigator_.onPress({MappedInputManager::Button::Right}, [this] { moveCursor(1, 0); });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) handleConfirm();

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
  // Step, not the glyph box: the pieces are narrower than their box and overlap, so
  // a full 15-piece set still fits the row at this size.
  constexpr int step = chess_view::SMALL_GLYPH_SIZE - 8;
  int x = right - leadWidth - (leadWidth > 0 ? 6 : 0) - count * step;
  x = std::max(x, left + pageWidth / 3);
  // Stand the pieces on the name's baseline instead of the row's top edge. Every
  // glyph in the set is drawn with 2 px of clearance below it inside its box
  // (scripts/gen_chess_pieces.py gives them all one base line), so subtracting that
  // puts the piece's own foot where the text sits rather than a third of a row above.
  constexpr int glyphBasePad = 2;
  const int glyphY = y + renderer.getFontAscenderSize(UI_12_FONT_ID) - glyph + glyphBasePad;
  for (int i = 0; i < count; i++) {
    chess_view::drawPiece(renderer, captured[i], x, glyphY, glyph);
    x += step;
  }
  if (leadText[0] != '\0') {
    renderer.drawText(UI_10_FONT_ID, right - leadWidth, y + (nameHeight - renderer.getLineHeight(UI_10_FONT_ID)),
                      leadText, true, EpdFontFamily::BOLD);
  }
  (void)metrics;
}

void ChessGameActivity::drawMoveList(int y, int height) const {
  const int left = layout_.x;
  const int right = layout_.x + layout_.size;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int blockTop = y;

  renderer.fillRect(left, y, right - left, 1, true);
  y += 8;

  const int plies = game_->plyCount();
  // The whole status line lives in the record now, as the next unplayed half-move:
  // "15. Nxh8  Thinking.." is one thing to read instead of a scoresheet and a
  // caption underneath repeating half of it, and the panel gets that line back.
  // Counted as a pair so the window sizing and scrolling below reserve a row for it.
  const char* pendingText = nullptr;
  if (state_ != State::GameOver) {
    if (state_ == State::Thinking || !playerToMove()) {
      pendingText = tr(STR_CHESS_THINKING);
    } else if (game_->board().inCheck(game_->board().whiteToMove())) {
      pendingText = tr(STR_CHESS_CHECK);
    } else {
      pendingText = tr(STR_CHESS_YOUR_MOVE);
    }
  }
  const int pendingPly = (pendingText != nullptr) ? plies : -1;
  const int pairs = (plies + (pendingPly >= 0 ? 2 : 1)) / 2;
  const int visiblePairs = std::max(1, std::min((height - 12) / lineHeight, pairs));

  // Keep the pair the record cursor sits on inside the window, scrolling it to the
  // bottom as the cursor walks forward and to the top as it walks back.
  int firstPair = std::max(0, pairs - visiblePairs);
  if (focus_ == Focus::Record) {
    const int cursorPair = std::max(0, (reviewPly_ - 1) / 2);
    firstPair = std::clamp(firstPair, std::max(0, cursorPair - visiblePairs + 1), cursorPair);
  }

  for (int pair = firstPair; pair < std::min(pairs, firstPair + visiblePairs); pair++) {
    char number[8];
    snprintf(number, sizeof(number), "%d.", pair + 1);
    const int numberWidth = renderer.getTextWidth(UI_12_FONT_ID, number);

    // The record being previewed is inverted, so "which move produced this board"
    // is answerable from the list rather than from the ply counter alone.
    const int whitePly = pair * 2;
    const bool whiteCurrent = (focus_ == Focus::Record && reviewPly_ == whitePly + 1);
    const bool blackCurrent = (focus_ == Focus::Record && reviewPly_ == whitePly + 2);
    if (whiteCurrent) renderer.fillRect(left + 38, y - 2, 88, lineHeight, true);
    if (blackCurrent) renderer.fillRect(left + 128, y - 2, 88, lineHeight, true);

    renderer.drawText(UI_12_FONT_ID, left + 32 - numberWidth, y, number, true);
    if (whitePly < plies) {
      renderer.drawText(UI_12_FONT_ID, left + 42, y, game_->sanAt(whitePly), !whiteCurrent);
    } else if (whitePly == pendingPly) {
      renderer.drawText(UI_12_FONT_ID, left + 42, y, pendingText, true, EpdFontFamily::BOLD);
    }
    if (whitePly + 1 < plies) {
      renderer.drawText(UI_12_FONT_ID, left + 132, y, game_->sanAt(whitePly + 1), !blackCurrent);
    } else if (whitePly + 1 == pendingPly) {
      renderer.drawText(UI_12_FONT_ID, left + 132, y, pendingText, true, EpdFontFamily::BOLD);
    }
    y += lineHeight;
  }

  // Focus box around the whole block, the same affordance a selected list row gets:
  // the move list is a thing you have stepped onto, not just a thing being drawn.
  if (focus_ != Focus::Board) {
    renderer.drawRect(left - 4, blockTop + 4, right - left + 8, std::max(y - blockTop, lineHeight + 8), 2, true);
  }
}

void ChessGameActivity::drawOverlay() const {
  if (state_ != State::GameOver && state_ != State::QuitMenu && state_ != State::ResignConfirm) return;

  const int pageWidth = renderer.getScreenWidth();
  const int boxWidth = std::min(360, layout_.size);
  const int boxX = layout_.x + (layout_.size - boxWidth) / 2;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int rowHeight = UITheme::getInstance().getMetrics().listRowHeight;

  char title[64];
  char detail[80];
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
    optionCount = OVER_OPTION_COUNT;
    options[0] = tr(STR_CHESS_PLAY_AGAIN);
    options[1] = tr(STR_CHESS_REVIEW);
    options[2] = tr(STR_CHESS_BACK_TO_MENU);
  } else if (state_ == State::QuitMenu) {
    snprintf(title, sizeof(title), "%s", tr(STR_CHESS_PAUSED));
    detail[0] = '\0';
    optionCount = QUIT_OPTION_COUNT;
    options[0] = tr(STR_CHESS_RESUME);
    options[1] = tr(STR_CHESS_SAVE_EXIT);
    options[2] = tr(STR_CHESS_RESIGN);
  } else {
    snprintf(title, sizeof(title), "%s", tr(STR_CHESS_RESIGN));
    snprintf(detail, sizeof(detail), "%s", tr(STR_CHESS_RESIGN_CONFIRM));
    optionCount = RESIGN_OPTION_COUNT;
    options[0] = tr(STR_CHESS_KEEP_PLAYING);
    options[1] = tr(STR_CHESS_RESIGN);
  }

  const int detailLines = (detail[0] != '\0' ? 1 : 0);
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
  highlights.cursor = (focus_ == Focus::Board && state_ == State::Playing && playerToMove()) ? cursor_ : -1;
  highlights.selected = (focus_ == Focus::Board) ? selected_ : -1;
  if (focus_ == Focus::Record) {
    // The move that produced the position on screen, not the game's last move.
    if (reviewPly_ > 0) {
      const chess::Move& shown = game_->moveAt(reviewPly_ - 1);
      highlights.lastFrom = shown.from;
      highlights.lastTo = shown.to;
    }
  } else {
    highlights.lastFrom = lastFrom_;
    highlights.lastTo = lastTo_;
    highlights.legalTargets = (selected_ >= 0) ? legalTargets_ : nullptr;
  }
  chess_view::drawBoard(renderer, visibleBoard(), layout_, highlights, flipped_);

  const int bottomBarY = layout_.y + layout_.size + 6;
  drawPlayerBar(bottomBarY, /*opponentRow=*/false);
  const int moveListTop = bottomBarY + barHeight + 2;
  // The status caption used to sit under this block; folding it into the record
  // hands its line back to the record itself.
  drawMoveList(moveListTop, hintsTop - moveListTop - 8);

  drawOverlay();

  const char* backLabel = (selected_ >= 0) ? tr(STR_CANCEL) : tr(STR_BACK);
  const char* confirmLabel = (selected_ >= 0) ? tr(STR_CHESS_MOVE) : tr(STR_SELECT);
  // Left/Right only mean anything on the board; blanked off it so the bar does not
  // advertise two buttons that do nothing in the move list.
  const char* leftLabel = tr(STR_DIR_LEFT);
  const char* rightLabel = tr(STR_DIR_RIGHT);
  if (focus_ == Focus::Record) {
    backLabel = tr(STR_CHESS_BOARD);
    confirmLabel = tr(STR_CHESS_RESIGN);
    leftLabel = tr(STR_CHESS_PREV);
    rightLabel = tr(STR_CHESS_NEXT);
  } else if (state_ == State::GameOver || state_ == State::QuitMenu || state_ == State::ResignConfirm) {
    backLabel = tr(STR_BACK);
    confirmLabel = tr(STR_SELECT);
  }
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, leftLabel, rightLabel,
                                            /*rtlSwap=*/false);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Up/down are on the side buttons, which have no slot in the four-button bar. Drawn
  // at the buttons themselves, as dictionary word select does, rather than spelled out
  // in a line of text the user has to map back onto the hardware. The board never
  // reaches these edges -- MAX_CELL caps it at 448 px, well inside the 30 px hint
  // columns -- so nothing is painted over. On the record the same two buttons step
  // through the game, so they are labelled for what they do there.
  if (focus_ == Focus::Record) {
    GUI.drawSideButtonHints(renderer, tr(STR_CHESS_PREV), tr(STR_CHESS_NEXT));
  } else {
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }

  // Periodic HALF refresh: FAST leaves residue, and a dithered board shows it.
  if (++rendersSinceFullRefresh_ >= 20) {
    rendersSinceFullRefresh_ = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer();
  }
}
