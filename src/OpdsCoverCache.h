#pragma once

#include <string>

struct OpdsEntry;

// Cache path for an OPDS entry's cover, keyed by the entry's stable id (not
// its signed, time-limited download URL). .crosspoint/opds_covers/<hash>_<w>x<h>.bmp
std::string getOpdsCoverCachePath(const std::string& entryId, int width, int height);

// Downloads and converts entry.coverUrl to a cached 1-bit BMP at
// getOpdsCoverCachePath(entry.id, width, height) if not already cached.
// Returns true if a cached cover exists (already present or freshly fetched).
bool ensureOpdsCoverCached(const OpdsEntry& entry, const std::string& username, const std::string& password, int width,
                           int height);

// Deletes the entire OPDS cover cache directory, forcing every cover to be re-downloaded
// on next view. Covers that downloaded successfully once are never re-validated against
// the server afterward (see ensureOpdsCoverCached's "already cached" fast path), so a
// scheme or URL change on the server side (http/https, signed-URL format, etc.) won't
// self-heal existing entries -- this lets the user force a clean slate from Settings.
// Returns true if the directory was removed (or never existed).
bool clearOpdsCoverCache();
