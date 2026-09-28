#pragma once

#include <cstddef>
#include <cstdint>

// Affordability rule for HomeActivity's cover-tile frame snapshot.
//
// The snapshot is a pure redraw-cost cache: it holds the already-rendered cover
// pixels so a re-render (selector move, clock tick) can blit them back instead of
// re-decoding the thumbnail BMPs from SD. Covers are drawn identically either way,
// so refusing the snapshot costs SD reads, never the design.
//
// Its size is set by the theme's cover-tile height and the panel geometry, and it
// scales badly on the taller X3 panel: the Foulad tile (640px, the height its
// status line + 300px hero + Recents divider + 210px thumb row actually occupy)
// needs 42,768 bytes on an X3 in either portrait, and 51,282 bytes in either
// landscape -- against a measured clean-boot free heap of 49,040 with a largest
// free block of 38,900. So the portrait request cannot be served at all (it
// exceeds the largest block even on a pristine heap), and the landscape one would
// leave nothing behind if it could be. Buying it would starve everything that has
// to coexist with it on the Home screen, most visibly FontDecompressor's ~12KB
// per-group glyph buffers.
//
// Hence a reserve-based rule rather than a fixed cap: what matters is not the
// snapshot's absolute size but how much heap survives it. RESERVE_BYTES is sized
// so the largest working buffer Home legitimately needs afterwards still fits --
// the font decompressor's biggest observed hot group is ~12.2KB, and Home's
// own popup/label rendering and RecentBooksStore strings account for the rest.
// Cover *generation* (a 32KB PNG inflate ring buffer plus an EPUB metadata parse)
// deliberately releases the snapshot first, so it does not have to fit here.
//
// The snapshot is taken in horizontal bands rather than one block, because the
// single-block form could not be served on an X3: 42,768 bytes needed against a
// largest block of 42,996 left 228 bytes behind it, which is exactly the
// starvation BLOCK_HEADROOM_BYTES exists to refuse. Free heap was 100,116 at the
// time -- the heap had the space, just not in one piece. Bands are the same total
// against a much smaller per-allocation demand.
namespace HomeCoverBudget {

// Free heap that must remain after the snapshot is taken.
constexpr uint32_t RESERVE_BYTES = 24u * 1024u;
// Headroom demanded on top of the request inside the largest free block, so the
// snapshot never lands as the single block that leaves the allocator with nothing.
constexpr uint32_t BLOCK_HEADROOM_BYTES = 2u * 1024u;

// Total cost against total free heap. Applies to the whole snapshot however many
// pieces it is cut into: splitting changes which blocks are needed, not how much
// heap is consumed.
constexpr bool fitsTotal(const size_t total, const uint32_t freeHeap) {
  return total != 0 && static_cast<uint64_t>(total) + RESERVE_BYTES <= freeHeap;
}

// One allocation against the largest free block.
constexpr bool fitsBlock(const size_t block, const uint32_t maxAllocHeap) {
  return block != 0 && static_cast<uint64_t>(block) + BLOCK_HEADROOM_BYTES <= maxAllocHeap;
}

// `needed` bytes as a SINGLE allocation.
constexpr bool fits(const size_t needed, const uint32_t freeHeap, const uint32_t maxAllocHeap) {
  return fitsTotal(needed, freeHeap) && fitsBlock(needed, maxAllocHeap);
}

}  // namespace HomeCoverBudget
