#pragma once

#include <cstdint>

// Suppression policy for a heap that has just refused a block.
//
// An allocator that cannot serve N bytes cannot serve anything >= N either, until
// something is freed. Code that mallocs once per glyph (see FontDecompressor::
// getBitmap) otherwise re-issues the same impossible request for every remaining
// glyph on the line: on an X3 down to ~4KB free that produced 144 consecutive
// failures across five distinct sizes, 18 of them inside a single millisecond,
// each one fragmenting a little further on the way out.
//
// Keying suppression on the REQUEST SIZE rather than on the identity of the thing
// being allocated (a (font, group) pair, say) is what makes the burst converge: a
// pair-keyed latch is evicted by the very next glyph belonging to a different
// group and re-attempts everything, whereas a size floor only ever re-attempts a
// STRICTLY SMALLER request. That bounds the malloc calls after the first failure
// by the number of distinct decreasing sizes -- five, for the log above -- instead
// of by the number of glyphs.
//
// Deliberately NOT cleared by an unrelated smaller allocation succeeding: a 4KB
// success says nothing about whether a 12KB block exists, and treating it as
// evidence reintroduces the ping-pong the floor is there to stop. The floor is a
// suppression window, not a blacklist, so it IS cleared by reset() at every point
// where the owner actually hands memory back (FontDecompressor::freeHotGroup /
// freePageBuffer / deinit, i.e. every clearCache and so every page turn).
struct AllocFloor {
  // Smallest byte count the allocator has refused since the last reset().
  uint32_t floorBytes = UINT32_MAX;

  // True when a request of `needed` bytes is already known to be unserviceable.
  bool suppresses(uint32_t needed) const { return needed >= floorBytes; }

  // Record that malloc(needed) returned nullptr.
  void noteFailure(uint32_t needed) {
    if (needed < floorBytes) floorBytes = needed;
  }

  // Memory has been returned to the heap; re-arm.
  void reset() { floorBytes = UINT32_MAX; }

  bool isSuppressing() const { return floorBytes != UINT32_MAX; }
};
