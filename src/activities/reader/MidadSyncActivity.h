#pragma once

#include <string>

#include "FouladReadingPosition.h"
#include "activities/Activity.h"

/**
 * Cross-device reading position sync with Midad, reached from the reader drawer
 * (EINK_PAGE_SYNC_TASKS.md §3).
 *
 * Deliberately a separate activity rather than a network call inside the reader,
 * mirroring KOReaderSyncActivity: the reader releases Epub and Section (~65KB)
 * before replacing itself with this, because a TLS handshake cannot be afforded
 * alongside a loaded book. See EpubReaderActivity::launchMidadSync().
 *
 * Ends by returning to the reader either way. An accepted jump is handed over as
 * APP_STATE.pendingSyncJumpPercent rather than applied here -- resolving a
 * percentage to a spine and page needs the Epub this activity just released.
 */
class MidadSyncActivity final : public Activity {
 public:
  explicit MidadSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                             std::string fouladBookId, std::string bookTitle, std::string bookAuthor,
                             float progressPercent, int page, int totalPages, uint32_t readAtAgeSeconds)
      : Activity("MidadSync", renderer, mappedInput),
        epubPath(std::move(epubPath)),
        fouladBookId(std::move(fouladBookId)),
        bookTitle(std::move(bookTitle)),
        bookAuthor(std::move(bookAuthor)),
        progressPercent(progressPercent),
        page(page),
        totalPages(totalPages),
        readAtAgeSeconds(readAtAgeSeconds) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Sleeping mid-handshake strands the sync with the radio up and nothing on
  // screen to explain it; the prompt holds the device awake for the same reason
  // OtaUpdateActivity does while waiting on a decision.
  bool preventAutoSleep() override {
    return state == State::Connecting || state == State::Syncing || state == State::Resolving;
  }
  const char* activityDebugName() const override { return "MidadSyncActivity"; }

 private:
  enum class State : uint8_t {
    Connecting,  // WifiSelectionActivity is up
    Syncing,     // POST in flight
    UpToDate,    // synced, nothing newer elsewhere
    Prompt,      // account is ahead: offer the jump
    Resolving,   // searching the catalog for this book's id
    Failed,
    // No catalog id for this book, so there is nothing to sync against. Reached
    // without touching WiFi -- the answer is already known.
    NotInLibrary,
  };

  std::string epubPath;
  std::string fouladBookId;
  std::string bookTitle;
  std::string bookAuthor;
  float progressPercent = 0.0f;
  int page = -1;
  int totalPages = -1;
  uint32_t readAtAgeSeconds = 0;

  State state = State::Connecting;
  FouladReadingPosition::Position remote;
  // 0 = Jump, 1 = Sync and close. Defaults to Jump because the user pressed Sync, but
  // it is a prompt and not an action: someone who deliberately flicked back three
  // pages to re-read a paragraph must not have the device take it away.
  int promptSelection = 0;

  void onWifiSelectionComplete(bool success);
  // Finds this book's catalog id by searching the library, for a book that was
  // never opened through Library and so never had one recorded. Returns false when
  // no unambiguous match exists. See the implementation for why the match is on the
  // downloader's own filename rule rather than on title text.
  bool resolveBookId();
  void performSync();
  void acceptJump();
  void returnToReader();
  // Finishes the sync and puts the book down -- see the prompt handling.
  void closeBook();
};
