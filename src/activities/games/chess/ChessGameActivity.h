#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "ChessBoardView.h"
#include "ChessGame.h"
#include "ChessSearch.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// The board screen: player against one of the engine personas.
//
// The search runs on its own FreeRTOS task against a COPY of the position, so
// the main task can keep rendering and reading buttons while the engine
// thinks, and neither task ever touches the other's board.
class ChessGameActivity final : public Activity {
 public:
  ChessGameActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int level, bool playerIsWhite, bool resume)
      : Activity("ChessGame", renderer, mappedInput), level_(level), playerIsWhite_(playerIsWhite), resume_(resume) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == State::Thinking; }
  const char* activityDebugName() const override { return "ChessGameActivity"; }

 private:
  enum class State : uint8_t { Playing, Thinking, GameOver, QuitMenu };

  static constexpr int QUIT_OPTION_COUNT = 3;  // resume, save & exit, resign
  static constexpr int OVER_OPTION_COUNT = 3;  // play again, review, menu
  static constexpr uint32_t LONG_PRESS_MS = 500;
  // The engine recurses through negamax + quiescence; 8 KB is the same order
  // as the network tasks and leaves headroom over the measured high-water mark.
  static constexpr uint32_t ENGINE_STACK_BYTES = 8192;

  static void engineTaskTrampoline(void* context);
  void runEngine();
  bool startEngineTask();
  void stopEngineTask();

  void beginEngineTurn();
  void applyPlayerMove(uint8_t from, uint8_t to);
  void refreshLegalTargets();
  void moveCursor(int fileDelta, int rankDelta);
  void handleConfirm();
  void finishGameIfOver();
  void persist();
  void reportOutcome();
  bool playerToMove() const;

  void drawPlayerBar(int y, bool opponentRow) const;
  void drawMoveList(int y) const;
  void drawOverlay() const;

  std::unique_ptr<chess::Game> game_;
  std::unique_ptr<chess::Search> search_;
  chess::Board searchBoard_;  // the engine task's private copy
  chess::Move engineMove_{};

  TaskHandle_t engineTask_ = nullptr;
  SemaphoreHandle_t engineDoneSem_ = nullptr;
  volatile bool engineFinished_ = false;

  ButtonNavigator buttonNavigator_;
  chess_view::Layout layout_{};
  uint8_t legalTargets_[64] = {};

  int level_ = 2;
  bool playerIsWhite_ = true;
  bool resume_ = false;
  bool flipped_ = false;
  State state_ = State::Playing;
  int cursor_ = -1;
  int selected_ = -1;
  int lastFrom_ = -1;
  int lastTo_ = -1;
  int overlayIndex_ = 0;
  int outcome_ = 0;  // +1 player win, 0 draw, -1 loss; set once at game over
  bool outcomeReported_ = false;
  bool backHandled_ = false;
  // Counts renders so a HALF refresh can clear accumulated ghosting; FAST
  // refreshes leave residue that a dithered board makes obvious.
  int rendersSinceFullRefresh_ = 0;
};
