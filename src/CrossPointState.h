#pragma once
#include <PersistableStore.h>

#include <cstdint>
#include <mutex>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  mutable std::mutex _mutex;

  // Private constructor for singleton (see PersistableStore.h)
  CrossPointState() = default;
  ~CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  // Access the state mutex for protecting multi-field reads/writes from other cores.
  std::mutex& getMutex() const { return _mutex; }

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
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

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);

  bool saveToFile() const;

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
