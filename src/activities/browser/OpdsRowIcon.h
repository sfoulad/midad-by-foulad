#pragma once

#include <string_view>

// Which icon a navigation row on a catalogue page gets. Pure string logic over
// the entry's OPDS href, kept out of the activity (and free of any renderer or
// icon-bitmap dependency) so it is host-testable -- the activity maps these
// kinds onto the actual 32px bitmaps.
//
// Keyed on the href, never on the title: the server localises titles (the
// Library landing shows Arabic titles on an Arabic account), so title matching
// would silently give every row the generic icon in one language and the right
// one in another. Route shapes come from docs/opds-server-reference.md:
//
//   /opds/                          root navigation feed
//   /opds/books                     every book, paginated
//   /opds/recent                    every book, newest first
//   /opds/category/{slug}           one category's navigation feed
//   /opds/category/{slug}/all       that category's books
//   /opds/category/{slug}/recent    that category's books, newest first
//   /opds/search?q=...              search results
//
// Anything unrecognised is Collection, not a miss: a row the server added that
// this firmware has never heard of still reads as a shelf you can open, which
// is exactly what it is.
namespace OpdsRowIcon {

enum class Kind {
  None,        // no icon; the row's own text already says what it is
  Search,      // the synthetic search row
  AllBooks,    // /opds/books, /opds/category/{slug}/all
  Recent,      // /opds/recent, /opds/category/{slug}/recent
  Collection,  // a category/language shelf, and any unrecognised navigation row
  Book,        // a book entry rendered as a text row (a feed with no cover art)
};

// Path-only suffix test: ignores any query string and any trailing slash, so
// "/opds/recent?page=3" and "http://host/opds/recent/" both match "/recent".
inline bool pathEndsWith(std::string_view href, const std::string_view suffix) {
  const auto query = href.find('?');
  if (query != std::string_view::npos) href = href.substr(0, query);
  while (!href.empty() && href.back() == '/') href.remove_suffix(1);
  return href.size() >= suffix.size() && href.compare(href.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// `isSearchRow` / `isBook` come from the entry's OpdsEntryType; `isPageControl`
// is true for the synthetic "« Previous Page" / "Next Page »" rows, which the
// browser inserts from the feed's own rel="next"/rel="previous" links.
inline Kind classify(const std::string_view href, const bool isSearchRow, const bool isBook, const bool isPageControl) {
  if (isSearchRow) return Kind::Search;
  if (isPageControl) return Kind::None;
  if (isBook) return Kind::Book;
  if (pathEndsWith(href, "/recent")) return Kind::Recent;
  if (pathEndsWith(href, "/books")) return Kind::AllBooks;
  if (pathEndsWith(href, "/all")) return Kind::AllBooks;
  return Kind::Collection;
}

}  // namespace OpdsRowIcon
