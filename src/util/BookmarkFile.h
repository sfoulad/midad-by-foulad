#pragma once
#include <string>
#include <vector>

#include "../BookmarkEntry.h"

// Per-book bookmark persistence. Takes the book's path; bookmark-file path
// derivation (BookmarkUtil) and directory creation are hidden inside.
//
// Adopted from upstream CrossPoint's own JsonSettingsIO -> PersistableStore
// migration (commit 63eda54e) rather than hand-written, per
// docs/upstream-sync-architecture.md's Phase B: this closed the
// JsonSettingsIO.cpp/.h merge conflict outright instead of carrying a
// permanently-diverged remnant of that file for bookmarks alone.
namespace BookmarkFile {

// Loads the bookmarks for bookPath. The vector is cleared first; a missing or
// empty bookmark file yields an empty list and returns false.
bool load(const std::string& bookPath, std::vector<BookmarkEntry>& bookmarks);

// Saves the bookmarks for bookPath, creating the bookmarks directory as needed.
bool save(const std::string& bookPath, const std::vector<BookmarkEntry>& bookmarks);

}  // namespace BookmarkFile
