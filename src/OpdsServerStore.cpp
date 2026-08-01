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

    // Normalise a stored Midad catalog URL to whatever the current constant is.
    //
    // Originally this only swapped the host for the Midad rename. That was not
    // enough the moment the scheme changed too: every device in the field has
    // "http://midad.one/opds" on its SD card, and this entry -- not the constant --
    // is what requests use. Worse, "is this account set up?" is decided by comparing
    // it against FOULAD_EBOOKS_URL (goToFouladEbooks, the reader's sync gating, the
    // Settings login row), so a scheme change alone would make every existing device
    // conclude it was signed out and demand a fresh QR scan.
    //
    // Replacing the whole URL rather than patching pieces means the next change --
    // host, scheme, path -- needs no new migration. isFouladEbooksUrl() matches both
    // the current and pre-rename hosts, which is why FOULAD_EBOOKS_LEGACY_HOST still
    // has to exist even though nothing calls foulad.one any more: without it these
    // entries stop being recognised as ours and never get rewritten at all.
    //
    // Credential, token flag and account are untouched -- same server, same account,
    // different address.
    if (!server.url.empty() && server.url != FOULAD_EBOOKS_URL && isFouladEbooksUrl(server.url)) {
      LOG_INF("OPS", "Migrated stored catalog URL: %s -> %s", server.url.c_str(), FOULAD_EBOOKS_URL);
      server.url = FOULAD_EBOOKS_URL;
      server.name = FOULAD_EBOOKS_NAME;
      needsResave = true;
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
