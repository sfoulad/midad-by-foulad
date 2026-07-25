#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct OpdsServer {
  std::string name;
  std::string url;
  std::string username;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
  // True when `password` holds a server-issued per-device token (QR sign-in,
  // see FouladDeviceLogin) rather than the user's real account password. Both
  // are sent identically as HTTP Basic Auth, so nothing downstream cares -- this
  // exists so a later migration can find the installs still sending a real
  // password over the non-TLS OPDS connection and offer to re-pair them.
  // Recorded at sign-in time because it cannot be recovered afterwards: a token
  // is not reliably distinguishable from a password by inspection.
  // Absent from older opds.json files, where it correctly defaults to false.
  bool isDeviceToken = false;
};

/**
 * Singleton class for storing OPDS server configurations on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON.
 */
class OpdsServerStore : public PersistableStore<OpdsServerStore> {
 private:
  std::vector<OpdsServer> servers;

  static constexpr size_t MAX_SERVERS = 8;

  OpdsServerStore() = default;

  friend class PersistableStore<OpdsServerStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/opds.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool addServer(const OpdsServer& server);
  bool updateServer(size_t index, const OpdsServer& server);
  bool removeServer(size_t index);

  const std::vector<OpdsServer>& getServers() const { return servers; }
  const OpdsServer* getServer(size_t index) const;
  size_t getCount() const { return servers.size(); }
  bool hasServers() const { return !servers.empty(); }
};

#define OPDS_STORE OpdsServerStore::getInstance()
