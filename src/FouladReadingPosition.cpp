#include "FouladReadingPosition.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip unless seen
// first. Same pinned order as OtaUpdater.cpp.
#include "network/HttpDownloader.h"
#include <ArduinoJson.h>
#include <HalGPIO.h>
#include <Logging.h>
// clang-format on

#include <cstdlib>

#include "FouladContentHash.h"
#include "FouladDeviceTracking.h"
#include "FouladEbooksConfig.h"
#include "network/CoreCaCerts.h"

namespace {
constexpr const char* TAG = "FRP";

}  // namespace

namespace {
// Core returns one position object under different keys: "position" on a PUT
// (what we just stored) and "resume" on a GET (its canonical answer across all
// this account's devices).
bool parseCorePosition(JsonVariantConst obj, const std::string& ourDeviceId, const bool serverSawConflict,
                       FouladReadingPosition::Position& out) {
  if (obj.isNull()) {
    out.hasPosition = false;
    return true;  // a book nobody has opened yet is a valid answer, not a failure
  }
  out.hasPosition = true;
  out.progressPercent = obj["progressPercent"].as<float>();
  out.page = obj["pageNumber"].isNull() ? -1 : obj["pageNumber"].as<int>();
  out.totalPages = obj["pageTotal"].isNull() ? -1 : obj["pageTotal"].as<int>();
  out.deviceName = obj["deviceName"].isNull() ? "" : obj["deviceName"].as<std::string>();

  // deviceKind is Core's vocabulary (xteink | foulad_apple | koreader | web);
  // the UI only distinguishes "this kind of thing" from "a phone".
  const std::string kind = obj["deviceKind"].isNull() ? "" : obj["deviceKind"].as<std::string>();
  out.source = (kind == "xteink") ? "reader" : "app";

  // Deliberately an identity test, not a percentage comparison. The header's rule
  // is that this device must not re-derive the server's float logic -- but "was
  // this written by somebody else" needs no float logic, and Core does not send a
  // should_jump flag on this contract. Our own last position is never a jump.
  const std::string owner = obj["deviceId"].isNull() ? "" : obj["deviceId"].as<std::string>();
  // A conflict means Core could not honestly order the top two devices (they read
  // within 900 s of each other and disagree by >= 0.5%, or a clock was untrusted).
  // Jumping silently on that is the one thing the contract forbids: it would move
  // the reader to a position the server itself will not vouch for. Staying put is
  // always safe -- the local position is real, just possibly not the newest.
  out.shouldJump = !owner.empty() && owner != ourDeviceId && !serverSawConflict;
  if (serverSawConflict) LOG_INF(TAG, "server reported a conflict; not jumping");

  // Core's spineIndex is 1-BASED (resume contract, "spineIndex: 4 // 1-BASED").
  // Everything downstream here is 0-based: jumpToSpine() treats index 0 as valid
  // and index == spineCount as out of range. Passing the wire value straight
  // through would land every jump one document late, and refuse outright on the
  // final one. Convert once, here, so only this line knows about the difference.
  const int wireSpine = obj["spineIndex"].isNull() ? 0 : obj["spineIndex"].as<int>();
  out.spineIndex = wireSpine >= 1 ? wireSpine - 1 : -1;
  out.spineCount = obj["spineCount"].isNull() ? -1 : obj["spineCount"].as<int>();
  return true;
}

std::string positionUrl(const std::string& bookId) {
  return std::string(FOULAD_CORE_RESUME_ITEM_URL) + bookId + "/position";
}
}  // namespace

bool FouladReadingPosition::sync(const std::string& username, const std::string& password,
                                 const std::string& fouladBookId, const std::string& bookPath,
                                 const float progressPercent, const int page, const int totalPages,
                                 const uint32_t readAtEpochSeconds, const uint32_t readAtAgeSeconds, Position& out) {
  (void)readAtEpochSeconds;  // Core takes the age; see the header on why absolute time is worse
  if (username.empty() || password.empty() || fouladBookId.empty()) return false;
  if (!FouladDeviceTracking::wifiConnected()) return false;

  // REQUIRED by the contract, and the reason this is not a two-line repoint: a
  // position means nothing without the bytes it was taken against. Cached per
  // book, so the full read pass happens once rather than on every page turn.
  const std::string contentSha = FouladContentHash::get(bookPath);
  if (contentSha.empty()) {
    LOG_ERR(TAG, "no content hash for %s; not reporting position", bookPath.c_str());
    return false;
  }

  const std::string deviceId = FouladDeviceTracking::getSerialNumber();

  JsonDocument doc;
  doc["deviceId"] = deviceId;
  doc["deviceKind"] = "xteink";
  doc["deviceName"] = gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4";
  doc["contentSha256"] = contentSha;
  doc["contentFormat"] = "epub";
  // The reader has no CFI and no xpointer to offer. Claiming a precision we do not
  // have would make a phone jump to a place we never computed; "percent" is the
  // honest locator kind for what this device actually knows.
  doc["locatorKind"] = "percent";
  char locator[16];
  snprintf(locator, sizeof(locator), "%.2f", static_cast<double>(progressPercent));
  doc["locator"] = locator;
  doc["progressPercent"] = progressPercent;
  if (page > 0) doc["pageNumber"] = page;
  if (totalPages > 0) doc["pageTotal"] = totalPages;
  if (page > 0 || totalPages > 0) doc["pageSource"] = "crosspoint";
  if (readAtAgeSeconds > 0) doc["readAtAgeSeconds"] = readAtAgeSeconds;

  std::string body;
  serializeJson(doc, body);

  std::string response;
  const auto err = HttpDownloader::putJsonWolf(positionUrl(fouladBookId), body, core_ca::kMidadCoreCaGtsRootR4,
                                               response, 15000, username, password);
  if (err != HttpDownloader::OK) {
    const int status = HttpDownloader::getLastFailure().detail;
    if (status == 404) {
      // Core resolves the Store id against THIS account's own acquisition. A book
      // sideloaded, or downloaded before this account was on Core OPDS, has no
      // acquisition row and never will -- so this is terminal, not a transient
      // failure to retry. Same 404 as a foreign or trashed item, by design.
      LOG_ERR(TAG, "book %s is not an acquisition of this account; position dropped", fouladBookId.c_str());
      return false;
    }
    LOG_ERR(TAG, "position PUT failed (err=%d, http=%d)", static_cast<int>(err), status);
    return false;
  }

  JsonDocument reply;
  if (deserializeJson(reply, response) != DeserializationError::Ok) {
    LOG_ERR(TAG, "Malformed position PUT response");
    return false;
  }
  out.resolution = reply["outcome"].isNull() ? "" : reply["outcome"].as<std::string>();
  LOG_INF(TAG, "position sent: %.2f%% -> %s", static_cast<double>(progressPercent), out.resolution.c_str());
  return parseCorePosition(reply["position"], deviceId, /*serverSawConflict=*/false, out);
}

bool FouladReadingPosition::fetch(const std::string& username, const std::string& password,
                                  const std::string& fouladBookId, const std::string& bookPath, Position& out) {
  if (username.empty() || password.empty() || fouladBookId.empty()) return false;
  if (!FouladDeviceTracking::wifiConnected()) return false;

  const std::string contentSha = FouladContentHash::get(bookPath);
  if (contentSha.empty()) {
    LOG_ERR(TAG, "no content hash for %s; not asking for a position", bookPath.c_str());
    return false;
  }

  const std::string url = positionUrl(fouladBookId) + "?contentSha256=" + contentSha;
  std::string response;
  const bool ok = HttpDownloader::fetchUrlVerified(
      url,
      [&response](const uint8_t* data, const size_t len) {
        response.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      username, password);
  if (!ok) {
    LOG_ERR(TAG, "position GET failed (http=%d)", HttpDownloader::getLastFailure().detail);
    return false;
  }

  JsonDocument reply;
  if (deserializeJson(reply, response) != DeserializationError::Ok) {
    LOG_ERR(TAG, "Malformed position GET response");
    return false;
  }
  return parseCorePosition(reply["resume"], FouladDeviceTracking::getSerialNumber(),
                           /*serverSawConflict=*/!reply["conflict"].isNull(), out);
}
