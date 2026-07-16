#pragma once
#include <PersistableStore.h>

#include <cstdint>

// Persisted best-score tracking for the built-in games. Kept in its own small
// file rather than folded into CrossPointSettings: the settings JSON loop
// (JsonSettingsIO) only round-trips uint8_t fields (see SettingInfo::valuePtr),
// too narrow for a Tetris score in the thousands, and high scores aren't a
// user-configurable setting anyway -- same reasoning RecentBooksStore/
// BookmarkEntry already get their own files instead of living in Settings.
class GameHighScoresStore : public PersistableStore<GameHighScoresStore> {
 private:
  uint32_t snakeHighScore = 0;
  uint32_t tetrisHighScore = 0;

  GameHighScoresStore() = default;
  ~GameHighScoresStore() = default;

  friend class PersistableStore<GameHighScoresStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/game_scores.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  uint32_t getSnakeHighScore() const { return snakeHighScore; }
  uint32_t getTetrisHighScore() const { return tetrisHighScore; }

  // Records a just-finished game's score against the stored best. Returns
  // true (and persists) only when it's a new record, so callers can show a
  // "New Best!" style message without a separate comparison.
  bool reportSnakeScore(uint32_t score);
  bool reportTetrisScore(uint32_t score);
};

#define GAME_SCORES GameHighScoresStore::getInstance()
