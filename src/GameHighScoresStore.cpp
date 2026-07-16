#include "GameHighScoresStore.h"

void GameHighScoresStore::toJson(JsonDocument& doc) const {
  doc["snakeHighScore"] = snakeHighScore;
  doc["tetrisHighScore"] = tetrisHighScore;
}

bool GameHighScoresStore::fromJson(JsonVariantConst doc) {
  snakeHighScore = doc["snakeHighScore"] | 0;
  tetrisHighScore = doc["tetrisHighScore"] | 0;
  return true;
}

bool GameHighScoresStore::reportSnakeScore(uint32_t score) {
  if (score <= snakeHighScore) return false;
  snakeHighScore = score;
  saveToFile();
  return true;
}

bool GameHighScoresStore::reportTetrisScore(uint32_t score) {
  if (score <= tetrisHighScore) return false;
  tetrisHighScore = score;
  saveToFile();
  return true;
}
