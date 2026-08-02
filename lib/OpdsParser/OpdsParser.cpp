#include "OpdsParser.h"

#include <Arduino.h>
#include <Logging.h>
#include <XmlParserUtils.h>

#include <cstring>

OpdsParser::OpdsParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
    return;
  }
  // Pre-reserve so entries.push_back() in endElement() doesn't trigger repeated
  // reallocate-copy-free growth cycles while the HTTP/TLS session is still active
  // and competing for the same heap. Confirmed via a real device crash: operator new
  // inside std::vector<OpdsEntry>::_M_realloc_append aborted (no exceptions to catch
  // under -fno-exceptions) well under the MAX_ENTRIES=200 cap -- the cap bounds the
  // final size, not the number/timing of reallocation events on the way there. 64
  // covers real category feeds seen so far (up to 26 entries) with no reallocation at
  // all; larger feeds still grow automatically (doubling) up to MAX_ENTRIES.
  //
  // But reserve(64) is itself a single ~11KB contiguous allocation (64 *
  // sizeof(OpdsEntry), each holding 7 std::string fields) -- with -fno-exceptions a
  // failed reserve() aborts the whole device, uncatchable. Confirmed via a second
  // real device crash: this exact reserve() call aborted at ~16KB free heap. Only
  // attempt it with comfortable headroom; below that, skip it and let the vector
  // grow via its own doubling from empty, where each growth allocation starts tiny
  // and stays far smaller than one 11KB block, so it's much more likely to find a
  // free contiguous run even when the heap is fragmented and low.
  constexpr uint32_t kMinFreeHeapForReserve = 64 * 1024;
  if (ESP.getFreeHeap() >= kMinFreeHeapForReserve) {
    entries.reserve(64);
  }
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return length;
    }

    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return length;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (errorOccured || !parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    destroyXmlParser(parser);
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  searchTemplate.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = false;
  truncated = false;
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) books.push_back(entry);
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        std::string sHref(href);
        if (sHref.find("{searchTerms}") != std::string::npos) {
          self->searchTemplate = sHref;
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        self->nextPageUrl = href;
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        self->prevPageUrl = href;
      }

      if (self->inEntry) {
        const bool isEpubAcquisition = type && strcmp(type, "application/epub+zip") == 0;
        // foulad-ebooks derives this MIME type from the stored file's own extension
        // (application/x-xtc or application/x-xtch) -- accept either, mirroring
        // FsHelpers::hasXtcExtension's "either spelling" treatment on the read side.
        const bool isXtcAcquisition =
            type && (strcmp(type, "application/x-xtc") == 0 || strcmp(type, "application/x-xtch") == 0);
        // The acquisition rel is the authoritative signal in OPDS: it means "this is
        // the file to download". Trust it even when the type is one we do not know.
        //
        // Requiring a known MIME type here is what made News unusable: its entries
        // arrived with an acquisition link the type check rejected, so they fell to
        // the atom branch below, became NAVIGATION, and the reader fetched the EPUB
        // and tried to parse it as a feed -- "Failed to parse feed" on every article
        // list. Downloading a file we cannot open is a better failure than pretending
        // it is a catalogue, and the extension logic already defaults sensibly.
        if (rel && strstr(rel, "opds-spec.org/acquisition") != nullptr) {
          // Prefer plain EPUB links over derived/other formats when multiple
          // acquisition links are present for one entry.
          // Known types still win when an entry offers several links: an unfamiliar
          // one is a fallback, not a preference.
          const bool isKnownType = isEpubAcquisition || isXtcAcquisition;
          const bool alreadyKnownType = self->currentEntry.type == OpdsEntryType::BOOK &&
                                        (self->currentEntry.acquisitionType == "application/epub+zip" ||
                                         self->currentEntry.acquisitionType == "application/x-xtc" ||
                                         self->currentEntry.acquisitionType == "application/x-xtch");
          const bool keepExisting = self->currentEntry.type == OpdsEntryType::BOOK && alreadyKnownType && !isKnownType;
          const bool isPlainEpub =
              isEpubAcquisition && (strstr(href, ".epub") != nullptr || strstr(href, "/epub/") != nullptr);
          const bool alreadyHasPlainEpub = self->currentEntry.type == OpdsEntryType::BOOK &&
                                           self->currentEntry.acquisitionType == "application/epub+zip" &&
                                           (self->currentEntry.href.find(".epub") != std::string::npos ||
                                            self->currentEntry.href.find("/epub/") != std::string::npos);
          if (!keepExisting &&
              (self->currentEntry.type != OpdsEntryType::BOOK || (isPlainEpub && !alreadyHasPlainEpub))) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            self->currentEntry.href = href;
            // type may be absent now that the rel alone qualifies; nullptr into a
            // std::string is undefined, not empty.
            self->currentEntry.acquisitionType = type ? type : "";
          }
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            self->currentEntry.href = href;
          }
        } else if (rel && strstr(rel, "opds-spec.org/image") != nullptr) {
          // Matches both ".../image" and ".../image/thumbnail". Prefer the
          // thumbnail-specific link if one is present; first-seen otherwise.
          const bool isThumbnail = strstr(rel, "thumbnail") != nullptr;
          if (self->currentEntry.coverUrl.empty() || isThumbnail) {
            self->currentEntry.coverUrl = href;
            self->currentEntry.coverType = type ? type : "";
          }
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
    return;
  }

  if (!self->inEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (!self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      if (self->entries.size() < MAX_ENTRIES) {
        self->entries.push_back(self->currentEntry);
      } else {
        if (!self->truncated) {
          LOG_ERR("OPDS", "Feed has more than %u entries; truncating (server should paginate)",
                  static_cast<unsigned>(MAX_ENTRIES));
        }
        self->truncated = true;
      }
    }
    self->inEntry = false;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = self->currentText;
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->inTitle || self->inAuthorName || self->inId) {
    self->currentText.append(s, len);
  }
}
