#include "GymCatalog.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

namespace GymCatalog {

bool loadFromSd(std::vector<GymCatalogEntry>& out) { return loadFromPath(CATALOG_PATH, out); }

bool loadFromPath(const std::string& path, std::vector<GymCatalogEntry>& out) {
  out.clear();

  HalFile file;
  if (!Storage.openFileForRead("GYM", path.c_str(), file)) {
    return false;  // Not downloaded yet -- expected until the user runs the download store.
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file);
  if (err) {
    LOG_ERR("GYM", "Catalog parse error: %s", err.c_str());
    return false;
  }

  const int version = doc["version"] | 0;
  if (version != 1) {
    LOG_ERR("GYM", "Unsupported gym catalog version: %d", version);
    return false;
  }

  JsonArrayConst exercisesArr = doc["exercises"].as<JsonArrayConst>();
  out.reserve(std::min(exercisesArr.size(), MAX_CATALOG_ENTRIES));
  for (JsonObjectConst obj : exercisesArr) {
    if (out.size() >= MAX_CATALOG_ENTRIES) break;
    GymCatalogEntry entry;
    entry.slug = obj["slug"] | "";
    entry.name = obj["name"] | "";
    entry.bodyPart = obj["bodyPart"] | "";
    entry.equipment = obj["equipment"] | "";
    entry.imageSize = obj["imageSize"] | 0;
    // crc32 values routinely exceed INT32_MAX (e.g. 4220614924) -- `| 0`
    // deduces a signed int fallback type and ArduinoJson's extraction fails
    // silently for out-of-range values, always returning 0. Must extract as
    // uint32_t explicitly (same fix DictionaryDownloadActivity already needed
    // for its own crc32 field).
    entry.imageCrc32 = obj["imageCrc32"].is<uint32_t>() ? obj["imageCrc32"].as<uint32_t>() : 0;
    entry.imageWidth = obj["imageWidth"] | 0;
    entry.imageHeight = obj["imageHeight"] | 0;
    if (entry.slug.empty()) continue;
    out.push_back(std::move(entry));
  }

  LOG_DBG("GYM", "Loaded catalog: %u exercises", static_cast<unsigned>(out.size()));
  return true;
}

std::string imagePath(const std::string& slug) { return "/gym/images/" + slug + ".jpg"; }

std::string instructionsPath(const std::string& slug) { return "/gym/instructions/" + slug + ".json"; }

}  // namespace GymCatalog
