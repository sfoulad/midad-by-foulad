#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "GymCatalog.h"

// Streams the catalog.json body one exercise object at a time, via the
// generic StreamingJsonParser SAX engine -- same approach OtaUpdater/
// ReleaseJsonParser already use for the GitHub release JSON, and for the
// same reason: ArduinoJson DOM-parsing the whole ~162KB/873-entry document
// at once (the original implementation) materializes every entry's strings
// simultaneously and OOM's on real ESP32-C3 hardware.
//
// Three modes, chosen by which constructor is used:
//  - Entry mode (`out` non-null): retains entries into `*out` (a deque, not
//    vector -- see GymCatalog.h). If `bodyPartFilter` is non-empty, only
//    entries whose bodyPart matches are retained; others are parsed and
//    discarded. Used by GymCatalog::loadExercisesForBodyPart -- always pass
//    a filter in practice, since retaining the FULL catalog this way still
//    failed on real hardware (even deque's own internal chunk-map growth
//    threw bad_alloc holding all ~873 entries at once; confirmed via device
//    crash log).
//  - Summary mode (`summaryOut` non-null): retains no per-exercise data at
//    all, just tallies a count per distinct bodyPart into `*summaryOut`
//    (bounded to the ~17 distinct body parts in the dataset). Used by
//    GymCatalog::loadBodyPartCounts.
//  - Count-only mode (both null): retains nothing, just validates and counts
//    every entry seen. Used by GymCatalog::validate.
//
// `getCount()` always reflects the total valid entries SEEN (ignoring any
// filter), matching validate()'s contract.
class GymCatalogJsonParser {
 public:
  explicit GymCatalogJsonParser(std::deque<GymCatalogEntry>* out, std::string bodyPartFilter = {});
  explicit GymCatalogJsonParser(std::vector<GymBodyPartCount>* summaryOut);

  GymCatalogJsonParser(const GymCatalogJsonParser&) = delete;
  GymCatalogJsonParser& operator=(const GymCatalogJsonParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  bool hasVersion() const { return versionFound; }
  int getVersion() const { return version; }
  size_t getCount() const { return committedCount; }

 private:
  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_EXERCISES_ARRAY,
    IN_EXERCISE_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    VERSION,
    EXERCISES,
    SLUG,
    NAME,
    BODY_PART,
    EQUIPMENT,
    IMAGE_SIZE,
    IMAGE_CRC32,
    IMAGE_WIDTH,
    IMAGE_HEIGHT,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void commitExercise();

  StreamingJsonParser parser;
  std::deque<GymCatalogEntry>* out;
  std::string bodyPartFilter;
  std::vector<GymBodyPartCount>* summaryOut;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t exerciseDepth;

  int version;
  bool versionFound;
  size_t committedCount;

  GymCatalogEntry current;
};
