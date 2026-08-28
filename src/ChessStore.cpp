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
  const JsonArray winsArray = doc["wins"].to<JsonArray>();
  const JsonArray lossesArray = doc["losses"].to<JsonArray>();
  const JsonArray drawsArray = doc["draws"].to<JsonArray>();
  for (int i = 0; i < chess::LEVEL_COUNT; i++) {
    winsArray.add(wins[i]);
    lossesArray.add(losses[i]);
    drawsArray.add(draws[i]);
  }
}

bool ChessStore::fromJson(JsonVariantConst doc) {
  // const char* rather than std::string: ArduinoJson's std::string converter
  // pulls a copy of the serializer into this TU (see PersistableStore's note).
  savedGame = doc["savedGame"] | "";
  if (savedGame.size() > MAX_SAVE_LENGTH) savedGame.clear();
  savedLevel = clampLevel(doc["savedLevel"] | 2);
  savedPlayerIsWhite = doc["savedPlayerIsWhite"] | true;

  JsonArrayConst winsArray = doc["wins"];
  JsonArrayConst lossesArray = doc["losses"];
  JsonArrayConst drawsArray = doc["draws"];
  for (int i = 0; i < chess::LEVEL_COUNT; i++) {
    wins[i] = (i < static_cast<int>(winsArray.size())) ? (winsArray[i] | 0) : 0;
    losses[i] = (i < static_cast<int>(lossesArray.size())) ? (lossesArray[i] | 0) : 0;
    draws[i] = (i < static_cast<int>(drawsArray.size())) ? (drawsArray[i] | 0) : 0;
  }
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

void ChessStore::reportResult(uint8_t level, int outcome) {
  const uint8_t index = clampLevel(level);
  if (outcome > 0) {
    wins[index]++;
  } else if (outcome < 0) {
    losses[index]++;
  } else {
    draws[index]++;
  }
  savedGame.clear();  // a finished game is not resumable
  saveToFile();
}

uint16_t ChessStore::getWins(uint8_t level) const { return wins[clampLevel(level)]; }
uint16_t ChessStore::getLosses(uint8_t level) const { return losses[clampLevel(level)]; }
uint16_t ChessStore::getDraws(uint8_t level) const { return draws[clampLevel(level)]; }

int ChessStore::strongestBeaten() const {
  for (int i = chess::LEVEL_COUNT - 1; i >= 0; i--) {
    if (wins[i] > 0) return i;
  }
  return -1;
}
