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
