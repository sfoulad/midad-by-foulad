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
  enum class State : uint8_t { Playing, Thinking, GameOver, QuitMenu, ResignConfirm };

  // Where the four buttons are pointing. The board and the record share one
  // vertical axis: pressing Down on the bottom rank steps off the board and onto
  // the record, the same way the reader's drawer steps from its tab strip into its
  // rows. On the record the four buttons become Board / Resign / Prev / Next, and
  // Prev/Next walk the board back through the game -- one focus, not a highlighted
  // state and a scrolling state, because there was nothing to do in the first but
  // enter the second.
  enum class Focus : uint8_t { Board, Record };

  static constexpr int QUIT_OPTION_COUNT = 3;    // resume, save & exit, resign
  static constexpr int OVER_OPTION_COUNT = 3;    // play again, review, menu
  static constexpr int RESIGN_OPTION_COUNT = 2;  // keep playing, resign
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
  // Moves the cursor onto the player's own king. Used on the way back from the
  // move list, so the board always resumes on a piece rather than wherever the
  // cursor happened to be parked.
  void cursorToOwnKing();
  // Steps onto the record, entering it at the live position so the first Prev is a
  // step backwards through the game rather than a jump somewhere else.
  void enterRecord();
  void leaveRecord();
  // The board actually drawn: the live position, or the replayed one while the
  // record is being walked back through.
  const chess::Board& visibleBoard() const;
  void finishGameIfOver();
  void persist();
  void reportOutcome();
  bool playerToMove() const;

  void drawPlayerBar(int y, bool opponentRow) const;
  void drawMoveList(int y, int height) const;
  void drawOverlay() const;

  std::unique_ptr<chess::Game> game_;
  std::unique_ptr<chess::Search> search_;
  chess::Board searchBoard_;  // the engine task's private copy
  chess::Board reviewBoard_;  // replayed position while Focus::Record is active
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
  // Board orientation, set once from the player's colour so their own pieces are
  // always nearest. Not a toggle any more: the side buttons that used to flip it
  // now move the cursor.
  bool flipped_ = false;
  State state_ = State::Playing;
  Focus focus_ = Focus::Board;
  // Ply the record cursor sits on, i.e. the number of plies played in the position
  // on screen. Only meaningful in Focus::Record.
  int reviewPly_ = 0;
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
