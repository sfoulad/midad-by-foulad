#include "KOReaderCredentialStore.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>

namespace {
// Default sync server URL
constexpr char DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";
}  // namespace

void KOReaderCredentialStore::toJson(JsonDocument& doc) const {
  doc["username"] = getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(getPassword());
  doc["serverUrl"] = getServerUrl();
  doc["matchMethod"] = static_cast<uint8_t>(getMatchMethod());
  doc["syncBehavior"] = static_cast<uint8_t>(getSyncBehavior());
  if (pendingSync) {
    JsonObject p = doc["pendingSync"].to<JsonObject>();
    p["documentHash"] = pendingSync->documentHash;
    p["xpath"] = pendingSync->xpath;
    p["percentage"] = pendingSync->percentage;
    p["spineIndex"] = pendingSync->spineIndex;
    p["pageNumber"] = pendingSync->pageNumber;
    p["totalPages"] = pendingSync->totalPages;
    if (pendingSync->paragraphIndex) {
      p["paragraphIndex"] = *pendingSync->paragraphIndex;
    }
  }
}

bool KOReaderCredentialStore::fromJson(JsonVariantConst doc) {
  std::string user = doc["username"] | "";

  bool needsResave = false;
  std::string pass = extractPassword(doc, needsResave);

  setCredentials(user, pass);
  setServerUrl(doc["serverUrl"] | "");

  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  if (method <= static_cast<uint8_t>(DocumentMatchMethod::BINARY)) {
    setMatchMethod(static_cast<DocumentMatchMethod>(method));
  } else {
    LOG_DBG("KRS", "Invalid matchMethod %u in JSON, resetting to FILENAME", method);
    setMatchMethod(DocumentMatchMethod::FILENAME);
  }

  // Migration: a credential file predating this setting has no "syncBehavior" key and must
  // default to ASK_EVERY_TIME to preserve existing users' behavior. Only a brand-new store
  // (no file on disk yet, in-memory default from the class member initializer) gets SMART.
  const JsonVariantConst behaviorValue = doc["syncBehavior"];
  const bool missingBehavior = behaviorValue.isNull();
  uint8_t behavior = behaviorValue | static_cast<uint8_t>(KOReaderSyncBehavior::ASK_EVERY_TIME);
  if (behavior <= static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    setSyncBehavior(static_cast<KOReaderSyncBehavior>(behavior));
    needsResave = needsResave || missingBehavior;
  } else {
    LOG_DBG("KRS", "Invalid syncBehavior %u in JSON, resetting to ASK_EVERY_TIME", behavior);
    setSyncBehavior(KOReaderSyncBehavior::ASK_EVERY_TIME);
    needsResave = true;
  }

  const JsonVariantConst pendingValue = doc["pendingSync"];
  if (!pendingValue.isNull()) {
    PendingKOReaderSync pending;
    pending.documentHash = pendingValue["documentHash"] | "";
    pending.xpath = pendingValue["xpath"] | "";
    pending.percentage = pendingValue["percentage"] | 0.0f;
    pending.spineIndex = pendingValue["spineIndex"] | (uint16_t)0;
    pending.pageNumber = pendingValue["pageNumber"] | (uint16_t)0;
    pending.totalPages = pendingValue["totalPages"] | (uint16_t)1;
    // An empty hash means a prior partial/corrupt write; drop it rather than
    // later uploading progress attached to no document.
    if (!pending.documentHash.empty()) {
      const JsonVariantConst paraValue = pendingValue["paragraphIndex"];
      if (!paraValue.isNull()) {
        pending.paragraphIndex = paraValue.as<uint16_t>();
      }
      pendingSync = std::move(pending);
    }
  }

  if (needsResave) {
    LOG_DBG("KRS", "Resaved KOReader credentials to update format");
    saveToFile();
  }

  return true;
}

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
  LOG_DBG("KRS", "Set credentials for user: %s", user.c_str());
}

std::string KOReaderCredentialStore::getMd5Password() const {
  if (password.empty()) {
    return "";
  }

  // Calculate MD5 hash of password using ESP32's MD5Builder
  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();

  return md5.toString().c_str();
}

bool KOReaderCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }

void KOReaderCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
  LOG_DBG("KRS", "Cleared KOReader credentials");
}

void KOReaderCredentialStore::setServerUrl(const std::string& url) {
  serverUrl = url;
  LOG_DBG("KRS", "Set server URL: %s", url.empty() ? "(default)" : url.c_str());
}

std::string KOReaderCredentialStore::getBaseUrl() const {
  std::string url;
  if (serverUrl.empty()) {
    url = DEFAULT_SERVER_URL;
  } else if (serverUrl.find("://") == std::string::npos) {
    // Normalize URL: add http:// if no protocol specified (local servers typically don't have SSL)
    url = "http://" + serverUrl;
  } else {
    url = serverUrl;
  }

  // Strip trailing slashes to avoid double-slash in API paths
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  return url;
}

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) {
  matchMethod = method;
  LOG_DBG("KRS", "Set match method: %s", method == DocumentMatchMethod::FILENAME ? "Filename" : "Binary");
}

void KOReaderCredentialStore::setSyncBehavior(KOReaderSyncBehavior behavior) {
  if (static_cast<uint8_t>(behavior) > static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    behavior = KOReaderSyncBehavior::ASK_EVERY_TIME;
  }
  syncBehavior = behavior;
  LOG_DBG("KRS", "Set sync behavior: %s", behavior == KOReaderSyncBehavior::SMART ? "Smart" : "Ask");
}

void KOReaderCredentialStore::setPendingSync(const PendingKOReaderSync& pending) {
  pendingSync = pending;
  saveToFile();
  LOG_DBG("KRS", "Queued KOReader sync for next reconnect (doc=%s, %.1f%%)", pending.documentHash.c_str(),
          pending.percentage * 100.0f);
}

void KOReaderCredentialStore::clearPendingSync() {
  if (!pendingSync) return;
  pendingSync.reset();
  saveToFile();
}
