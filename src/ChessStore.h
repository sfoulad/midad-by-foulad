#pragma once
#include <PersistableStore.h>

#include <cstdint>

#include "ChessSearch.h"

// Saved chess game. Its own file rather than a field in
// CrossPointSettings for the same reason GameHighScoresStore is: this is game
// state, not a user-configurable setting, and the settings serializer only
// round-trips uint8_t fields.
class ChessStore : public PersistableStore<ChessStore> {
 private:
  // Base FEN plus the coordinate move list -- what chess::Game::toSaveString
  // produces. 200 plies of "e2e4 " is 1 KB, so the cap is generous but bounded.
  static constexpr size_t MAX_SAVE_LENGTH = 1200;

  std::string savedGame;
  uint8_t savedLevel = 2;
  bool savedPlayerIsWhite = true;

  ChessStore() = default;
  ~ChessStore() = default;

  friend class PersistableStore<ChessStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/chess.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool hasSavedGame() const { return !savedGame.empty(); }
  const std::string& getSavedGame() const { return savedGame; }
  uint8_t getSavedLevel() const { return savedLevel; }
  bool getSavedPlayerIsWhite() const { return savedPlayerIsWhite; }

  // Stores the in-progress game. Writes only when something actually changed,
  // so a move that ends in the same state does not cost an SD write.
  void saveGame(const char* serialized, uint8_t level, bool playerIsWhite);
  // Called when a game ends as well as when one is abandoned: a finished game is
  // not resumable, and no result is kept beyond that.
  void clearSavedGame();
};

#define CHESS_STORE ChessStore::getInstance()
