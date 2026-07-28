#include "OpdsServerStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>

#include "FouladEbooksConfig.h"

void OpdsServerStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : servers) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["url"] = server.url;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
    // Only written when true, so existing files gain nothing and older firmware
    // reading this file just ignores the key.
    if (server.isDeviceToken) obj["is_device_token"] = true;
  }
}

bool OpdsServerStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'servers' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  servers.clear();
  JsonArrayConst arr = doc["servers"].as<JsonArrayConst>();
  servers.reserve(std::min(arr.size(), MAX_SERVERS));
  bool needsResave = false;

  for (JsonObjectConst obj : arr) {
    if (servers.size() >= OpdsServerStore::MAX_SERVERS) break;
    OpdsServer server;
    server.name = obj["name"] | "";
    server.url = obj["url"] | "";
    server.username = obj["username"] | "";
    server.password = extractPassword(obj, needsResave);
    // Absent on every file written before QR sign-in existed, which is exactly
    // right: those credentials are typed account passwords.
    server.isDeviceToken = obj["is_device_token"] | false;

    // Midad rename migration. A device paired before the rename holds
    // "http://foulad.one/opds" here, and this stored entry -- not the constant --
    // is what every request uses, so without rewriting it the device would keep
    // talking to the old host indefinitely. Worse, ActivityManager::goToFouladEbooks()
    // decides "is this account set up?" by comparing this URL against
    // FOULAD_EBOOKS_URL: left alone, the comparison fails after the rename and the
    // device concludes it is signed out, throwing the user at a QR screen despite a
    // perfectly good credential.
    //
    // Host swap only. The credential, its is_device_token flag and the account
    // itself are untouched and stay valid -- the server is the same server under a
    // new name. needsResave persists it, so this runs once, not on every boot.
    if (!server.url.empty() && server.url.find(FOULAD_EBOOKS_LEGACY_HOST) != std::string::npos &&
        isFouladEbooksUrl(server.url)) {
      const size_t hostPos = server.url.find(FOULAD_EBOOKS_LEGACY_HOST);
      server.url.replace(hostPos, std::strlen(FOULAD_EBOOKS_LEGACY_HOST), FOULAD_EBOOKS_HOST);
      // The display name was "Foulad eBooks" on those entries; bring it along so the
      // server list doesn't show the old brand next to a migrated URL.
      server.name = FOULAD_EBOOKS_NAME;
      needsResave = true;
      LOG_INF("OPS", "Migrated stored catalog URL to %s", server.url.c_str());
    }

    servers.push_back(std::move(server));
  }

  LOG_DBG("OPS", "Loaded %zu OPDS servers from file", servers.size());

  if (needsResave) {
    LOG_DBG("OPS", "Resaving JSON with obfuscated passwords");
    saveToFile();
  }

  return true;
}

bool OpdsServerStore::addServer(const OpdsServer& server) {
  if (servers.size() >= MAX_SERVERS) {
    LOG_DBG("OPS", "Cannot add more servers, limit of %zu reached", MAX_SERVERS);
    return false;
  }

  servers.push_back(server);
  LOG_DBG("OPS", "Added server: %s", server.name.c_str());
  return saveToFile();
}

bool OpdsServerStore::updateServer(size_t index, const OpdsServer& server) {
  if (index >= servers.size()) {
    return false;
  }

  servers[index] = server;
  LOG_DBG("OPS", "Updated server: %s", server.name.c_str());
  return saveToFile();
}

bool OpdsServerStore::removeServer(size_t index) {
  if (index >= servers.size()) {
    return false;
  }

  LOG_DBG("OPS", "Removed server: %s", servers[index].name.c_str());
  servers.erase(servers.begin() + static_cast<ptrdiff_t>(index));
  return saveToFile();
}

const OpdsServer* OpdsServerStore::getServer(size_t index) const {
  if (index >= servers.size()) {
    return nullptr;
  }
  return &servers[index];
}
