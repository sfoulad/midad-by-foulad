#pragma once

#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>

// Where an OPDS cover lands on the SD card, keyed by the entry's stable id (not its
// signed, time-limited download URL) and by the size it was rendered for.
//
// Built through one fixed stack buffer and a single std::string construction. The
// path is rebuilt three times per cover (the page pre-scan, the fetch loop, and
// ensureOpdsCoverCached itself) on a heap a catalogue walk has already fragmented,
// and the chained operator+ this replaces churned roughly seven transient strings
// per call -- exactly the small-block traffic that starves the contiguous block the
// TLS handshake and the image decoder then need.
//
// Header-only and free of hardware dependencies so the format is covered by host
// tests (test/opds_cover_cache_path).
namespace OpdsCoverCachePath {

constexpr char DIR[] = "/.crosspoint/opds_covers";

// DIR (24) + '/' + up to 20 digits of size_t + '_' + two ints + 'x' + ".bmp" + NUL.
// Sized so snprintf never truncates: a truncated path would silently alias two
// different covers onto one cache file.
constexpr size_t MAX_LEN = 96;

inline std::string build(const std::string& entryId, const int width, const int height) {
  char path[MAX_LEN];
  const int written = snprintf(path, sizeof(path), "%s/%zu_%dx%d.bmp", DIR,
                               static_cast<size_t>(std::hash<std::string>{}(entryId)), width, height);
  if (written <= 0) return std::string();
  const size_t length = static_cast<size_t>(written) < sizeof(path) ? static_cast<size_t>(written) : sizeof(path) - 1;
  return std::string(path, length);
}

}  // namespace OpdsCoverCachePath
