#include "GymCatalogJsonParser.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

GymCatalogJsonParser::GymCatalogJsonParser(std::deque<GymCatalogEntry>* out, std::string bodyPartFilter)
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}),
      out(out),
      bodyPartFilter(std::move(bodyPartFilter)),
      summaryOut(nullptr) {
  reset();
}

GymCatalogJsonParser::GymCatalogJsonParser(std::vector<GymBodyPartCount>* summaryOut)
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}),
      out(nullptr),
      summaryOut(summaryOut) {
  reset();
}

void GymCatalogJsonParser::reset() {
  parser.reset();
  position = Position::TOP_LEVEL;
  lastKey = LastKey::NONE;
  depth = 0;
  exerciseDepth = 0;
  version = 0;
  versionFound = false;
  committedCount = 0;
  current = GymCatalogEntry{};
}

void GymCatalogJsonParser::feed(const char* data, size_t len) { parser.feed(data, len); }

void GymCatalogJsonParser::commitExercise() {
  if (!current.slug.empty()) {
    ++committedCount;
    if (summaryOut) {
      auto it = std::find_if(summaryOut->begin(), summaryOut->end(),
                             [this](const GymBodyPartCount& c) { return c.bodyPart == current.bodyPart; });
      if (it != summaryOut->end()) {
        ++it->count;
      } else {
        summaryOut->push_back({current.bodyPart, 1});
      }
    }
    if (out && (bodyPartFilter.empty() || current.bodyPart == bodyPartFilter) &&
        out->size() < GymCatalog::MAX_CATALOG_ENTRIES) {
      out->push_back(std::move(current));
    }
  }
  current = GymCatalogEntry{};
}

// -- SAX callbacks (static trampolines) -------------------------------------

void GymCatalogJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<GymCatalogJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth == 1) {
        if (len == 7 && memcmp(key, "version", 7) == 0)
          self->lastKey = LastKey::VERSION;
        else if (len == 9 && memcmp(key, "exercises", 9) == 0)
          self->lastKey = LastKey::EXERCISES;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    case Position::IN_EXERCISE_OBJECT:
      if (self->exerciseDepth == 1) {
        if (len == 4 && memcmp(key, "slug", 4) == 0)
          self->lastKey = LastKey::SLUG;
        else if (len == 4 && memcmp(key, "name", 4) == 0)
          self->lastKey = LastKey::NAME;
        else if (len == 8 && memcmp(key, "bodyPart", 8) == 0)
          self->lastKey = LastKey::BODY_PART;
        else if (len == 9 && memcmp(key, "equipment", 9) == 0)
          self->lastKey = LastKey::EQUIPMENT;
        else if (len == 9 && memcmp(key, "imageSize", 9) == 0)
          self->lastKey = LastKey::IMAGE_SIZE;
        else if (len == 10 && memcmp(key, "imageCrc32", 10) == 0)
          self->lastKey = LastKey::IMAGE_CRC32;
        else if (len == 10 && memcmp(key, "imageWidth", 10) == 0)
          self->lastKey = LastKey::IMAGE_WIDTH;
        else if (len == 11 && memcmp(key, "imageHeight", 11) == 0)
          self->lastKey = LastKey::IMAGE_HEIGHT;
        else
          self->lastKey = LastKey::NONE;
      }
      break;
    default:
      break;
  }
}

void GymCatalogJsonParser::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<GymCatalogJsonParser*>(ctx);

  if (self->position == Position::IN_EXERCISE_OBJECT && self->exerciseDepth == 1) {
    switch (self->lastKey) {
      case LastKey::SLUG:
        self->current.slug.assign(value, len);
        break;
      case LastKey::NAME:
        self->current.name.assign(value, len);
        break;
      case LastKey::BODY_PART:
        self->current.bodyPart.assign(value, len);
        break;
      case LastKey::EQUIPMENT:
        self->current.equipment.assign(value, len);
        break;
      default:
        break;
    }
  }
  self->lastKey = LastKey::NONE;
}

void GymCatalogJsonParser::sOnNumber(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<GymCatalogJsonParser*>(ctx);

  // strtoul (not atoi/ArduinoJson's `| 0`) so crc32 values above INT32_MAX
  // (e.g. 4220614924) parse correctly instead of silently truncating --
  // same bug DictionaryDownloadActivity's own crc32 field already needed
  // fixing for, on the ArduinoJson DOM path this replaces.
  if (self->position == Position::TOP_LEVEL && self->lastKey == LastKey::VERSION && self->depth == 1) {
    self->version = atoi(value);
    self->versionFound = true;
  } else if (self->position == Position::IN_EXERCISE_OBJECT && self->exerciseDepth == 1) {
    switch (self->lastKey) {
      case LastKey::IMAGE_SIZE:
        self->current.imageSize = static_cast<uint32_t>(strtoul(value, nullptr, 10));
        break;
      case LastKey::IMAGE_CRC32:
        self->current.imageCrc32 = static_cast<uint32_t>(strtoul(value, nullptr, 10));
        break;
      case LastKey::IMAGE_WIDTH:
        self->current.imageWidth = static_cast<uint16_t>(strtoul(value, nullptr, 10));
        break;
      case LastKey::IMAGE_HEIGHT:
        self->current.imageHeight = static_cast<uint16_t>(strtoul(value, nullptr, 10));
        break;
      default:
        break;
    }
  }
  (void)len;
  self->lastKey = LastKey::NONE;
}

void GymCatalogJsonParser::sOnBool(void* ctx, bool /*value*/) {
  static_cast<GymCatalogJsonParser*>(ctx)->lastKey = LastKey::NONE;
}

void GymCatalogJsonParser::sOnNull(void* ctx) { static_cast<GymCatalogJsonParser*>(ctx)->lastKey = LastKey::NONE; }

void GymCatalogJsonParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<GymCatalogJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      self->depth++;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_EXERCISES_ARRAY:
      self->position = Position::IN_EXERCISE_OBJECT;
      self->exerciseDepth = 1;
      self->current = GymCatalogEntry{};
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_EXERCISE_OBJECT:
      self->exerciseDepth++;
      self->lastKey = LastKey::NONE;
      break;
  }
}

void GymCatalogJsonParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<GymCatalogJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_EXERCISE_OBJECT:
      self->exerciseDepth--;
      if (self->exerciseDepth == 0) {
        self->commitExercise();
        self->position = Position::IN_EXERCISES_ARRAY;
      }
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void GymCatalogJsonParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<GymCatalogJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->lastKey == LastKey::EXERCISES && self->depth == 1) {
        self->position = Position::IN_EXERCISES_ARRAY;
      } else {
        self->depth++;
      }
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_EXERCISE_OBJECT:
      self->exerciseDepth++;
      self->lastKey = LastKey::NONE;
      break;
    default:
      break;
  }
}

void GymCatalogJsonParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<GymCatalogJsonParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_EXERCISES_ARRAY:
      self->position = Position::TOP_LEVEL;
      break;
    case Position::IN_EXERCISE_OBJECT:
      self->exerciseDepth--;
      self->lastKey = LastKey::NONE;
      break;
  }
}
