#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

// Parses either GET /releases/latest's response (a single release object) or
// GET /releases' response (an array of every release, newest-first, used to
// find a pre-release/RC -- /releases/latest specifically excludes those) --
// see OtaUpdater.cpp for which endpoint each mode hits. When fed an array,
// only the FIRST element (the newest release/pre-release) is parsed; once
// that element's own object closes, every further SAX event is ignored so a
// second (older) release in the array can't overwrite the first one's
// already-captured tag/firmware fields.
class ReleaseJsonParser {
 public:
  ReleaseJsonParser();

  ReleaseJsonParser(const ReleaseJsonParser&) = delete;
  ReleaseJsonParser& operator=(const ReleaseJsonParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  // Release-asset filename to match (default "firmware.bin"). Boards with
  // their own release binaries pass e.g. "firmware-papermono.bin". Survives
  // reset(); truncated silently if longer than the internal buffer.
  void setFirmwareAssetName(const char* name);

  bool foundTag() const;
  bool foundFirmware() const;
  const char* getTagName() const;
  const char* getFirmwareUrl() const;
  size_t getFirmwareSize() const;

 private:
  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_ASSETS_ARRAY,
    IN_ASSET_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    TAG_NAME,
    ASSETS,
    ASSET_NAME,
    ASSET_URL,
    ASSET_SIZE,
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

  void commitAsset();

  StreamingJsonParser parser;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t assetDepth;
  // True once the top-level JSON value has been seen to be an array (GET
  // /releases) rather than a single object (GET /releases/latest).
  bool topLevelIsArray;
  // True once the first (newest) release object in a top-level array has
  // fully closed -- every SAX event past that point is ignored.
  bool done;

  char tagName[32];
  char firmwareUrl[512];
  size_t firmwareSize;
  bool tagFound;
  bool firmwareFound;

  char currentAssetName[32];
  char currentAssetUrl[512];
  size_t currentAssetSize;

  char firmwareAssetName[32];
};
