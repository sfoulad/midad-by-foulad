#include "CrossPointState.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <mutex>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 4;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
}  // namespace

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < SLEEP_RECENT_COUNT; i++) recentArr.add(recentSleepImages[i]);
  doc["recentSleepPos"] = recentSleepPos;
  doc["recentSleepFill"] = recentSleepFill;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["pendingSyncJumpPercent"] = pendingSyncJumpPercent;
  doc["pendingSyncJumpSpine"] = pendingSyncJumpSpine;
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["showBootScreen"] = showBootScreen;
  doc["pendingBleBookFetchId"] = pendingBleBookFetchId;
}

bool CrossPointState::fromJson(JsonVariantConst doc) {
  openEpubPath = doc["openEpubPath"] | "";
  memset(recentSleepImages, 0, sizeof(recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount =
      recentArr.isNull() ? 0 : std::min(static_cast<int>(recentArr.size()), static_cast<int>(SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (recentSleepPos >= SLEEP_RECENT_COUNT) recentSleepPos = actualCount > 0 ? recentSleepPos % SLEEP_RECENT_COUNT : 0;
  recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(recentSleepFill), actualCount));
  // Migrate legacy single-image field from old state.json (pre-recency-buffer).
  // Only seeds the buffer if the new buffer is empty (fresh migration, not a resave).
  if (recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  // Absent on a state file written before cross-device sync; 0 means nothing pending.
  pendingSyncJumpPercent = doc["pendingSyncJumpPercent"] | static_cast<uint8_t>(0);
  pendingSyncJumpSpine = doc["pendingSyncJumpSpine"] | static_cast<int16_t>(-1);
  lastSleepFromReader = doc["lastSleepFromReader"] | false;
  showBootScreen = doc["showBootScreen"] | true;
  // Absent on a state file written before book.fetch existed; empty means nothing
  // pending, same convention as pendingSyncJumpPercent's 0 above.
  pendingBleBookFetchId = doc["pendingBleBookFetchId"] | "";
  return true;
}

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

bool CrossPointState::saveToFile() const {
  std::lock_guard<std::mutex> lock(_mutex);
  return PersistableStore<CrossPointState>::saveToFile();
}

bool CrossPointState::loadFromFile() {
  // Try JSON first. PersistableStore::loadFromFile() returns false uniformly
  // for a missing, empty, OR corrupt state.json -- a deliberate widening from
  // the pre-migration code, which only fell through to the binary migration
  // below for missing/empty. Recovering a corrupt state.json from
  // state.bin.bak (when one exists) is strictly better than discarding state,
  // so this is an intentional improvement, not an accidental behavior change.
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (PersistableStore<CrossPointState>::loadFromFile()) {
      return true;
    }
  }

  // Fall back to binary migration
  if (Storage.exists(STATE_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
        LOG_DBG("CPS", "Migrated state.bin to state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during migration");
        return false;
      }
    }
  }

  return false;
}

bool CrossPointState::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(_mutex);

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    uint8_t legacyLastSleep = UINT8_MAX;
    serialization::readPod(inputFile, legacyLastSleep);
    if (legacyLastSleep != UINT8_MAX) {
      pushRecentSleep(static_cast<uint16_t>(legacyLastSleep));
    }
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  return true;
}
