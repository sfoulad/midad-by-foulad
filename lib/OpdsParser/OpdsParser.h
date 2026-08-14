#pragma once
#include <Print.h>
#include <expat.h>

#include <string>
#include <vector>

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK,        // Downloadable book
  // Opens the search keyboard rather than fetching anything. Never produced by the
  // parser -- the browser synthesises one row per feed from the OpenSearch template
  // in <link rel="search">, so search is a thing you can see and select rather than
  // a button binding you have to be told about.
  SEARCH
};

/**
 * Represents an entry from an OPDS feed (either a navigation link or a book).
 */
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  std::string title;
  std::string author;  // Only for books
  std::string href;    // Navigation URL or book download URL
  std::string id;
  std::string coverUrl;   // From <link rel="http://opds-spec.org/image[/thumbnail]">, if present
  std::string coverType;  // MIME type of coverUrl, e.g. "image/jpeg"
  // MIME type of the acquisition link href, e.g. "application/epub+zip" or "application/x-xtc".
  // Only for books -- lets the downloader pick the right file extension/handling per format.
  std::string acquisitionType;
};

// Legacy alias for backward compatibility
using OpdsBook = OpdsEntry;

/**
 * Parser for OPDS (Open Publication Distribution System) Atom feeds.
 * Uses the Expat XML parser to parse OPDS catalog entries.
 *
 * Usage:
 *   OpdsParser parser;
 *   if (parser.parse(xmlData, xmlLength)) {
 *     for (const auto& entry : parser.getEntries()) {
 *       if (entry.type == OpdsEntryType::BOOK) {
 *         // Downloadable book
 *       } else {
 *         // Navigation link to another catalog
 *       }
 *     }
 *   }
 */
class OpdsParser final : public Print {
 public:
  // Hard cap on how many entries a single feed response can add to `entries`. A feed
  // server that doesn't paginate (returns every book in a category in one response) can
  // otherwise grow this vector until operator new fails -- with -fno-exceptions that's an
  // immediate abort(), not a catchable error (confirmed via a real device crash: hundreds
  // of books in one unpaginated OPDS feed exhausted the ~380KB heap while appending
  // OpdsEntry objects during XML parsing). Silently truncating is a safety net; the real
  // fix for a feed this large is server-side pagination (rel="next"/"previous" links,
  // which this parser already understands -- see nextPageUrl/prevPageUrl below).
  static constexpr size_t MAX_ENTRIES = 200;

  OpdsParser();
  ~OpdsParser();

  // Disable copy
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  OpdsParser(const OpdsParser&) = delete;
  OpdsParser& operator=(const OpdsParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;

  void flush() override;

  bool error() const;

  operator bool() { return !error(); }

  /**
   * Get the parsed entries (both navigation and book entries).
   * @return Vector of OpdsEntry entries
   */
  const std::vector<OpdsEntry>& getEntries() const& { return entries; }
  std::vector<OpdsEntry> getEntries() && { return std::move(entries); }

  // True if this feed had more entries than MAX_ENTRIES and got truncated. Lets the UI
  // tell the user their view is incomplete instead of silently dropping books.
  bool wasTruncated() const { return truncated; }

  /**
   * Get only book entries (legacy compatibility).
   * @return Vector of book entries
   */
  std::vector<OpdsEntry> getBooks() const;

  /**
   * Clear all parsed entries.
   */
  void clear();

 private:
  // Expat callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  std::string searchTemplate;
  std::string nextPageUrl;
  std::string prevPageUrl;
  // Helper to find attribute value
  static const char* findAttribute(const XML_Char** atts, const char* name);
  // Cap a single string field's length so a malformed/malicious feed can't drive
  // unbounded std::string growth into a throwing operator new -- the same abort
  // class MAX_ENTRIES above and the move-not-copy push_back below already guard
  // for the entry vector and per-entry copies; these close the same hole for one
  // oversized field within an otherwise-normal entry. assignBounded replaces the
  // target outright (attribute values, e.g. href); appendBounded accumulates
  // character data across possibly-chunked XML_CharacterDataHandler calls.
  static void assignBounded(std::string& target, const char* value, size_t maxLen);
  static void appendBounded(std::string& target, const char* value, size_t len, size_t maxLen);

  XML_Parser parser = nullptr;
  std::vector<OpdsEntry> entries;
  OpdsEntry currentEntry;
  std::string currentText;

  // Parser state
  bool inEntry = false;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;
  bool inId = false;
  bool collectCurrentEntry = false;

  bool errorOccured = false;
  bool truncated = false;
};
