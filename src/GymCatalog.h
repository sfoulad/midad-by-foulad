#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One row of the downloaded exercise catalog (see GYM_STORE_TASKS.md's
// GET /api/gym/catalog contract). Metadata only -- no instructions text (that
// and the image are fetched lazily, per-exercise, only once the user adds it
// to a day; see GymDownloadActivity, Phase 2).
struct GymCatalogEntry {
  std::string slug;
  std::string name;
  std::string bodyPart;
  std::string equipment;
  uint32_t imageSize = 0;
  uint32_t imageCrc32 = 0;
  uint16_t imageWidth = 0;
  uint16_t imageHeight = 0;
};

namespace GymCatalog {
// Locally-cached copy of the server catalog, written once by
// GymDownloadActivity after a successful fetch; read directly (no re-download)
// by the exercise browser every time it opens.
constexpr const char* CATALOG_PATH = "/gym/catalog.json";
// Safety cap on parse -- comfortably above the ~500-800 exercises the source
// dataset actually has (see GYM_STORE_TASKS.md PART 0); guards against a
// corrupt/hostile catalog file forcing an unbounded allocation.
constexpr size_t MAX_CATALOG_ENTRIES = 2000;

// Parses the locally-cached catalog.json into `out` (cleared first). Returns
// false if the file doesn't exist yet (nothing downloaded) or fails to parse
// -- same DOM-parse-the-whole-file convention as FontDownloadActivity/
// DictionaryDownloadActivity's manifest parse, held only for the caller's
// lifetime (not a persistent RAM-resident store).
bool loadFromSd(std::vector<GymCatalogEntry>& out);

// Same parse, from an arbitrary SD path -- used by GymCatalogSyncActivity to
// validate a freshly-downloaded temp file before committing it over the real
// CATALOG_PATH, so a bad/truncated download can never clobber a working
// catalog.
bool loadFromPath(const std::string& path, std::vector<GymCatalogEntry>& out);

// Local path an exercise's image is (or would be) downloaded to.
std::string imagePath(const std::string& slug);
// Local path an exercise's instructions text is (or would be) cached to.
std::string instructionsPath(const std::string& slug);
}  // namespace GymCatalog
