#include "CrossPointState.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 4;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";

bool isRecentIndex(const uint16_t* recentImages, uint8_t recentPos, uint8_t recentFill, uint16_t idx,
                    uint8_t checkCount) {
  const uint8_t effectiveCount = std::min(checkCount, recentFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot =
        (recentPos + CrossPointState::SLEEP_RECENT_COUNT - 1 - i) % CrossPointState::SLEEP_RECENT_COUNT;
    if (recentImages[slot] == idx) return true;
  }
  return false;
}

void pushRecentIndex(uint16_t* recentImages, uint8_t& recentPos, uint8_t& recentFill, uint16_t idx) {
  recentImages[recentPos] = idx;
  recentPos = (recentPos + 1) % CrossPointState::SLEEP_RECENT_COUNT;
  if (recentFill < CrossPointState::SLEEP_RECENT_COUNT) recentFill++;
}

}  // namespace

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < SLEEP_RECENT_COUNT; i++) recentArr.add(recentSleepImages[i]);
  doc["recentSleepPos"] = recentSleepPos;
  doc["recentSleepFill"] = recentSleepFill;
  JsonArray recentOverlayArr = doc["recentOverlaySleepImages"].to<JsonArray>();
  for (int i = 0; i < SLEEP_RECENT_COUNT; i++) recentOverlayArr.add(recentOverlaySleepImages[i]);
  doc["recentOverlaySleepPos"] = recentOverlaySleepPos;
  doc["recentOverlaySleepFill"] = recentOverlaySleepFill;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["pendingSyncJumpPercent"] = pendingSyncJumpPercent;
  doc["pendingSyncJumpSpine"] = pendingSyncJumpSpine;
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["showBootScreen"] = showBootScreen;
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

  memset(recentOverlaySleepImages, 0, sizeof(recentOverlaySleepImages));
  JsonArrayConst recentOverlayArr = doc["recentOverlaySleepImages"];
  const int actualOverlayCount = recentOverlayArr.isNull() ? 0
                                                            : std::min(static_cast<int>(recentOverlayArr.size()),
                                                                       static_cast<int>(SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualOverlayCount; i++) {
    recentOverlaySleepImages[i] = recentOverlayArr[i] | static_cast<uint16_t>(0);
  }
  recentOverlaySleepPos = doc["recentOverlaySleepPos"] | static_cast<uint8_t>(0);
  if (recentOverlaySleepPos >= SLEEP_RECENT_COUNT) {
    recentOverlaySleepPos = actualOverlayCount > 0 ? recentOverlaySleepPos % CrossPointState::SLEEP_RECENT_COUNT : 0;
  }
  recentOverlaySleepFill = doc["recentOverlaySleepFill"] | static_cast<uint8_t>(0);
  recentOverlaySleepFill = static_cast<uint8_t>(std::min(static_cast<int>(recentOverlaySleepFill), actualOverlayCount));

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
  return true;
}

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  return isRecentIndex(recentSleepImages, recentSleepPos, recentSleepFill, idx, checkCount);
}

bool CrossPointState::isRecentOverlaySleep(uint16_t idx, uint8_t checkCount) const {
  return isRecentIndex(recentOverlaySleepImages, recentOverlaySleepPos, recentOverlaySleepFill, idx, checkCount);
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  pushRecentIndex(recentSleepImages, recentSleepPos, recentSleepFill, idx);
}

void CrossPointState::pushRecentOverlaySleep(uint16_t idx) {
  pushRecentIndex(recentOverlaySleepImages, recentOverlaySleepPos, recentOverlaySleepFill, idx);
}

bool CrossPointState::loadFromFile() {
  // PersistableStore::loadFromFile() returns false uniformly for a missing,
  // empty, OR corrupt state.json -- a deliberate widening from the
  // pre-migration code, which only fell through to the binary migration
  // below for missing/empty. Recovering a corrupt state.json from
  // state.bin.bak (when one exists) is strictly better than discarding
  // state, so this is an intentional improvement, not an accidental
  // behavior change. Locking is handled by the base class -- see
  // PersistableStore.h.
  if (PersistableStore<CrossPointState>::loadFromFile()) {
    return true;
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
  std::lock_guard<std::mutex> lock(storeMutex);

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
