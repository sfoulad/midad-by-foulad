#include "ChessStore.h"

namespace {
uint8_t clampLevel(uint8_t level) {
  return level >= chess::LEVEL_COUNT ? static_cast<uint8_t>(chess::LEVEL_COUNT - 1) : level;
}
}  // namespace

void ChessStore::toJson(JsonDocument& doc) const {
  doc["savedGame"] = savedGame;
  doc["savedLevel"] = savedLevel;
  doc["savedPlayerIsWhite"] = savedPlayerIsWhite;
}

bool ChessStore::fromJson(JsonVariantConst doc) {
  // const char* rather than std::string: ArduinoJson's std::string converter
  // pulls a copy of the serializer into this TU (see PersistableStore's note).
  savedGame = doc["savedGame"] | "";
  if (savedGame.size() > MAX_SAVE_LENGTH) savedGame.clear();
  savedLevel = clampLevel(doc["savedLevel"] | 2);
  savedPlayerIsWhite = doc["savedPlayerIsWhite"] | true;
  // "wins"/"losses"/"draws" from an older file are dropped: the record is gone from
  // every screen, so keeping the counters up to date bought nothing.
  return true;
}

void ChessStore::saveGame(const char* serialized, uint8_t level, bool playerIsWhite) {
  const char* text = (serialized == nullptr) ? "" : serialized;
  if (savedGame == text && savedLevel == clampLevel(level) && savedPlayerIsWhite == playerIsWhite) return;
  savedGame = text;
  if (savedGame.size() > MAX_SAVE_LENGTH) savedGame.clear();
  savedLevel = clampLevel(level);
  savedPlayerIsWhite = playerIsWhite;
  saveToFile();
}

void ChessStore::clearSavedGame() {
  if (savedGame.empty()) return;
  savedGame.clear();
  saveToFile();
}
