#include "MidadSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsParser.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointState.h"
#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "reading/ReadingStatsStore.h"
#include "util/StringUtils.h"

void MidadSyncActivity::onEnter() {
  Activity::onEnter();

  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void MidadSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    // Backing out of WiFi selection is a decision, not a failure -- go straight
    // back to the book rather than showing an error for something deliberate.
    returnToReader();
    return;
  }
  // The radio is up for this activity's own sync anyway -- opportunistically
  // deliver any KOReader Sync progress queued from a book closed since the
  // last reconnect (see EpubReaderActivity::onExit()).
  FouladDeviceTracking::flushPendingKOReaderSync();
  // A book that was only ever opened from Home has no catalog id, even though it
  // may well be in the library. Rather than telling the user to go and open it from
  // Library -- which is a chore that exists only because of how the id happens to be
  // recorded -- look it up now, while the radio is already up.
  if (fouladBookId.empty()) {
    {
      RenderLock lock(*this);
      state = State::Resolving;
    }
    requestUpdateAndWait();
    if (!resolveBookId()) {
      RenderLock lock(*this);
      state = State::NotInLibrary;
      requestUpdate();
      return;
    }
  }

  {
    RenderLock lock(*this);
    state = State::Syncing;
  }
  requestUpdateAndWait();
  performSync();
}

namespace {
// Percent-encodes a search term. Same rule as the browser's own search.
std::string urlEncode(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (const unsigned char c : in) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Trailing digits of urn:opds-library:book:{id}. Mirrors OpdsBookBrowserActivity's
// own extractor -- kept identical so both paths agree on what an id is.
std::string extractId(const std::string& entryId) {
  size_t end = entryId.size();
  while (end > 0 && isdigit(static_cast<unsigned char>(entryId[end - 1]))) end--;
  return end == entryId.size() ? "" : entryId.substr(end);
}
}  // namespace

bool MidadSyncActivity::resolveBookId() {
  const auto& servers = OPDS_STORE.getServers();
  const auto srv =
      std::find_if(servers.begin(), servers.end(), [](const OpdsServer& s) { return s.url == FOULAD_EBOOKS_URL; });
  if (srv == servers.end() || bookTitle.empty()) return false;

  const std::string url = std::string(FOULAD_EBOOKS_URL) + "/search?q=" + urlEncode(bookTitle);
  OpdsParser parser;
  OpdsParserStream stream{parser};
  if (!HttpDownloader::fetchUrl(url, stream, srv->username, srv->password) || !parser) return false;

  // Match on the filename the downloader WOULD have produced, not on title text.
  // That string is how the local file got its name in the first place
  // (OpdsBookBrowserActivity), so an exact match means this is the same catalog
  // entry -- whereas matching titles loosely could link a position to the wrong
  // book, which is a worse outcome than not linking at all.
  const size_t slash = epubPath.find_last_of('/');
  const std::string localName = slash == std::string::npos ? epubPath : epubPath.substr(slash + 1);
  const size_t dot = localName.find_last_of('.');
  const std::string localStem = dot == std::string::npos ? localName : localName.substr(0, dot);

  std::string found;
  for (const auto& entry : parser.getBooks()) {
    const std::string stem =
        StringUtils::sanitizeFilename((entry.author.empty() ? "" : entry.author + " - ") + entry.title);
    if (stem != localStem) continue;
    const std::string id = extractId(entry.id);
    if (id.empty()) continue;
    if (!found.empty() && found != id) return false;  // ambiguous: refuse rather than guess
    found = id;
  }
  if (found.empty()) return false;

  // Record it both places so this never has to happen again: recents is what the
  // catalog path writes, and the stats store is what survives recents eviction.
  fouladBookId = found;
  RECENT_BOOKS.addBook(epubPath, bookTitle, bookAuthor, "", found);
  READING_STATS.setFouladBookId(epubPath, found);
  LOG_INF("SYNC", "Resolved catalog id %s for a book opened outside Library", found.c_str());
  return true;
}

void MidadSyncActivity::performSync() {
  const auto& servers = OPDS_STORE.getServers();
  const auto it =
      std::find_if(servers.begin(), servers.end(), [](const OpdsServer& s) { return s.url == FOULAD_EBOOKS_URL; });
  if (it == servers.end()) {
    RenderLock lock(*this);
    state = State::Failed;
    requestUpdate();
    return;
  }

  // Refresh the registration first, exactly as reportDeviceTrackingOnConnect() does
  // when entering the catalog. Without this a device that only ever syncs -- never
  // browsing Library after an update -- keeps whatever firmware version it last
  // registered with. Observed stale across three releases, which quietly misattributes
  // every debug log, since those carry no version line of their own to fall back to.
  FouladDeviceTracking::registerDevice(it->username, it->password);

  FouladReadingPosition::Position fetched;
  const bool ok = FouladReadingPosition::sync(it->username, it->password, fouladBookId, progressPercent, page,
                                              totalPages, /*readAtEpochSeconds=*/0, readAtAgeSeconds, fetched);

  RenderLock lock(*this);
  if (!ok) {
    state = State::Failed;
  } else {
    remote = fetched;
    // should_jump is the server's decision and carries its own slack allowance, so
    // it never proposes a jump to where you already are. Deliberately not
    // second-guessed here by comparing percentages: two renderers computing a
    // position from the same place never agree to the decimal.
    state = remote.shouldJump ? State::Prompt : State::UpToDate;
  }
  requestUpdate();
}

void MidadSyncActivity::acceptJump() {
  // Handed to the reader rather than applied here: resolving a percentage to a
  // spine and page needs the Epub, which was released before this activity started
  // so the handshake could fit in RAM. Rounded to whole percent because that is
  // what the handoff carries and what jumpToPercent() takes.
  APP_STATE.pendingSyncJumpPercent = static_cast<uint8_t>(remote.progressPercent + 0.5f);
  // Carried alongside, and preferred by the reader when present. -1 stays -1: the
  // server sends both null together when it has nothing to anchor on, and treating
  // that as spine 0 would jump to the front of the book.
  APP_STATE.pendingSyncJumpSpine = static_cast<int16_t>(remote.spineIndex);
  APP_STATE.saveToFile();
  LOG_INF("SYNC", "Accepted jump to %u%% spine=%d (from %s)", APP_STATE.pendingSyncJumpPercent, remote.spineIndex,
          remote.deviceName.empty() ? "another device" : remote.deviceName.c_str());
  returnToReader();
}

void MidadSyncActivity::closeBook() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // Home rather than the reader. The position is already on the server, so there is
  // nothing left to save, and a silent restart would only reload a book about to be
  // left anyway.
  silentRestart();
}

void MidadSyncActivity::returnToReader() {
  // Silent restart rather than a plain activity swap: the reader is about to reload
  // a book into a heap that has just held a TLS session, which is the fragmentation
  // KOReaderSyncActivity reboots for as well.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  silentRestartToReader();
}

void MidadSyncActivity::loop() {
  if (state == State::Prompt) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
        mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (promptSelection > 0) {
        promptSelection--;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
        mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (promptSelection < 1) {
        promptSelection++;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (promptSelection == 0) {
        acceptJump();
      } else {
        // Not "stay here": the position has already been sent by this point, and
        // the reason someone syncs is to carry on reading somewhere else. So the
        // second choice finishes the job and puts the book down, rather than
        // dropping them back into a book they have just handed over.
        closeBook();
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      returnToReader();  // backing out is not a choice between the two -- keep reading
    }
    return;
  }

  if (state == State::UpToDate || state == State::Failed || state == State::NotInLibrary) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      returnToReader();
    }
  }
}

void MidadSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textWidth = pageWidth - metrics.contentSidePadding * 2;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SYNC_MIDAD));

  const int top = (pageHeight - lineHeight) / 2;

  switch (state) {
    case State::Connecting:
    case State::Resolving:
    case State::Syncing:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SYNCING));
      break;

    case State::UpToDate:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_SYNC_UP_TO_DATE), true, EpdFontFamily::BOLD);
      break;

    case State::Failed:
      renderer.drawCenteredTextWrapped(UI_10_FONT_ID, top, textWidth, tr(STR_SYNC_FAILED), /*maxLines=*/3, true,
                                       EpdFontFamily::BOLD);
      break;

    case State::NotInLibrary:
      renderer.drawCenteredTextWrapped(UI_10_FONT_ID, top, textWidth, tr(STR_SYNC_NOT_IN_LIBRARY), /*maxLines=*/4, true,
                                       EpdFontFamily::BOLD);
      break;

    case State::Prompt: {
      // Naming the other device is far more convincing than "another device", which
      // is why the server sends device_name at all.
      char msg[128];
      snprintf(msg, sizeof(msg), tr(STR_SYNC_CONTINUE_FROM), static_cast<int>(remote.progressPercent + 0.5f),
               remote.deviceName.empty() ? tr(STR_SYNC_ANOTHER_DEVICE) : remote.deviceName.c_str());
      const int msgHeight =
          renderer.drawCenteredTextWrapped(UI_10_FONT_ID, top - lineHeight, textWidth, msg, /*maxLines=*/3);

      const int buttonY = top - lineHeight + msgHeight + metrics.verticalSpacing * 2;
      constexpr int buttonWidth = 120;
      constexpr int buttonSpacing = 24;
      const int startX = (pageWidth - (buttonWidth * 2 + buttonSpacing)) / 2;
      const auto drawChoice = [&](const int slot, const char* label) {
        const int x = startX + slot * (buttonWidth + buttonSpacing);
        if (promptSelection == slot) {
          const std::string framed = "[" + std::string(label) + "]";
          renderer.drawText(UI_10_FONT_ID, x, buttonY, framed.c_str());
        } else {
          renderer.drawText(UI_10_FONT_ID, x + 4, buttonY, label);
        }
      };
      drawChoice(0, tr(STR_SYNC_JUMP));
      drawChoice(1, tr(STR_SYNC_AND_CLOSE));
      break;
    }
  }

  const auto labels = state == State::Prompt
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                          : mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
