#pragma once

#include <string>

/**
 * Whole-file SHA-256 of a book, as Core's resume-position contract requires
 * (`contentSha256`, 64 lowercase hex). A position taken against different bytes
 * is not the same position, so the hash binds a reported location to the exact
 * file it was taken in.
 *
 * Computed once per book and cached on SD, because computing it is a full read
 * pass: doing that on every page turn would put seconds of SD I/O in the reader's
 * hot path. The cache lives beside the book's other cache files and is keyed the
 * same way, so deleting `.crosspoint/` re-derives it like everything else.
 */
namespace FouladContentHash {

// Returns 64 lowercase hex chars, or "" if the file cannot be read. Reads the
// cached value when one exists and the file's size still matches; otherwise
// streams the file and writes the cache.
std::string get(const std::string& bookPath);

}  // namespace FouladContentHash
