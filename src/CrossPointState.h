#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  // Private constructor for singleton (see PersistableStore.h)
  CrossPointState() = default;
  ~CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};
  uint8_t recentSleepPos = 0;
  uint8_t recentSleepFill = 0;
  uint16_t recentOverlaySleepImages[SLEEP_RECENT_COUNT] = {};
  uint8_t recentOverlaySleepPos = 0;
  uint8_t recentOverlaySleepFill = 0;
  uint8_t readerActivityLoadCount = 0;
  // Cross-device jump handed from the Midad sync activity to the reader
  // (EINK_PAGE_SYNC_TASKS.md). The sync replaces the reader to free ~65KB for the
  // TLS handshake, so the accepted percentage cannot simply be passed in a call --
  // it survives here across the swap and is consumed once on the reader's next open.
  //
  // Deliberately a percentage rather than a resolved spine/page: turning one into
  // the other is EpubReaderActivity::jumpToPercent()'s job, and it needs the Epub
  // loaded to do it -- which is exactly what the sync activity had to release.
  // 0 = nothing pending; a jump to 0% is indistinguishable from none and is also
  // the one jump nobody needs.
  uint8_t pendingSyncJumpPercent = 0;
  // Spine anchor for that jump, or -1 when the server had none to give. Preferred
  // over the percentage: a whole-number percent resolves to about one spine item on
  // a 103-document book, so it cannot be more accurate than that however correct
  // both sides are. The percentage is kept as the fallback and to place within the
  // document once opened.
  int16_t pendingSyncJumpSpine = -1;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;
  bool isRecentOverlaySleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  void pushRecentOverlaySleep(uint16_t idx);

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
};

#define APP_STATE CrossPointState::getInstance()
