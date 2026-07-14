#include "FontDecompressor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <cstdlib>

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() {
  freePageBuffer();
  freeHotGroup();
  free(alignedOffsetTable);
  alignedOffsetTable = nullptr;
  alignedOffsetTableFont = nullptr;
  alignedOffsetTableSize = 0;
}

void FontDecompressor::clearCache() {
  freePageBuffer();
  freeHotGroup();
  // alignedOffsetTable is deliberately NOT cleared here: it depends only on the font's
  // static group/glyph layout, never on the per-page prewarm/hot-group state this
  // function resets, so it stays valid (and cheap to reuse) across every page turn for
  // as long as the same font is active. See ensureAlignedOffsetTable().
}

void FontDecompressor::freePageBuffer() {
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    free(pageSlots[s].buffer);
    free(pageSlots[s].glyphs);
    pageSlots[s] = {};
  }
  pageSlotCount = 0;
}

void FontDecompressor::freeHotGroup() {
  free(hotGroup);
  hotGroup = nullptr;
  hotGroupCapacity = 0;
  hotGroupFont = nullptr;
  hotGroupIndex = UINT16_MAX;
  free(hotGlyphBuf);
  hotGlyphBuf = nullptr;
  hotGlyphBufCapacity = 0;
}

bool FontDecompressor::ensureCapacity(uint8_t*& buf, uint32_t& capacity, uint32_t needed) {
  if (capacity >= needed) return true;
  // Grow-only, free-then-malloc: every caller fully rewrites the buffer after a grow, so the
  // old contents are dead -- freeing first gives the allocator its best shot on a tight heap.
  free(buf);
  buf = static_cast<uint8_t*>(malloc(needed));  // owned by FontDecompressor, freed in freeHotGroup()
  capacity = buf ? needed : 0;
  return buf != nullptr;
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex) {
  // O(1) path for frequency-grouped fonts with glyphToGroup mapping
  if (fontData->glyphToGroup != nullptr) {
    return fontData->glyphToGroup[glyphIndex];
  }

  // Contiguous-group fonts: linear scan
  for (uint16_t i = 0; i < fontData->groupCount; i++) {
    uint32_t first = fontData->groups[i].firstGlyphIndex;
    if (glyphIndex >= first && glyphIndex < first + fontData->groups[i].glyphCount) {
      return i;
    }
  }
  return fontData->groupCount;  // sentinel = not found
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf,
                                       uint32_t outSize) {
  const EpdFontGroup& group = fontData->groups[groupIndex];

  const uint32_t tDecomp = millis();
  inflateReader.init(false);
  inflateReader.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  if (!inflateReader.read(outBuf, outSize)) {
    stats.decompressTimeMs += millis() - tDecomp;
    LOG_ERR("FDC", "Decompression failed for group %u", groupIndex);
    return false;
  }
  stats.decompressTimeMs += millis() - tDecomp;
  return true;
}

// --- Byte-aligned helpers ---

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;

  auto accumGlyph = [&](const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      offset += ((g.width + 3) / 4) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: scan glyphs before glyphIndex that belong to this group
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(fontData->glyph[i]);
      }
    }
  } else {
    // Contiguous-group: sum aligned sizes of preceding glyphs in the group
    const EpdFontGroup& group = fontData->groups[groupIndex];
    for (uint32_t i = group.firstGlyphIndex; i < glyphIndex; i++) {
      accumGlyph(fontData->glyph[i]);
    }
  }

  return offset;
}

// Precompute getAlignedOffset() for every glyph in the font in one O(totalGlyphs) pass,
// instead of paying that scan's per-glyph cost (O(glyphIndex) for a frequency-grouped
// font) on every hot-group fallback lookup in getBitmap(). No-ops if fontData already
// matches the cached table (the common case: the same font serves every lookup for a
// whole reading session). Returns false only on allocation failure, in which case the
// caller falls back to the direct per-call computation.
bool FontDecompressor::ensureAlignedOffsetTable(const EpdFontData* fontData) {
  if (alignedOffsetTableFont == fontData && alignedOffsetTable) return true;

  free(alignedOffsetTable);
  alignedOffsetTable = nullptr;
  alignedOffsetTableFont = nullptr;
  alignedOffsetTableSize = 0;

  if (fontData->intervalCount == 0) return false;
  const auto& lastInterval = fontData->intervals[fontData->intervalCount - 1];
  const uint32_t totalGlyphs = lastInterval.offset + (lastInterval.last - lastInterval.first + 1);
  if (totalGlyphs == 0) return false;

  auto* table = static_cast<uint32_t*>(malloc(totalGlyphs * sizeof(uint32_t)));
  if (!table) {
    LOG_ERR("FDC", "OOM allocating %lu-entry aligned-offset table", (unsigned long)totalGlyphs);
    return false;
  }

  auto accumGlyph = [](uint32_t& offset, const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      offset += ((g.width + 3) / 4) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: one running offset per group, single pass over all glyphs.
    auto perGroupOffset = makeUniqueNoThrow<uint32_t[]>(fontData->groupCount);
    if (!perGroupOffset) {
      free(table);
      LOG_ERR("FDC", "OOM allocating %u-entry per-group offset tracker", fontData->groupCount);
      return false;
    }
    for (uint32_t i = 0; i < totalGlyphs; i++) {
      const uint16_t gi = fontData->glyphToGroup[i];
      table[i] = perGroupOffset[gi];
      accumGlyph(perGroupOffset[gi], fontData->glyph[i]);
    }
  } else {
    // Contiguous-group: offset resets to 0 at each group's firstGlyphIndex.
    for (uint16_t gi = 0; gi < fontData->groupCount; gi++) {
      const EpdFontGroup& group = fontData->groups[gi];
      uint32_t offset = 0;
      for (uint32_t i = 0; i < group.glyphCount; i++) {
        const uint32_t glyphI = group.firstGlyphIndex + i;
        table[glyphI] = offset;
        accumGlyph(offset, fontData->glyph[glyphI]);
      }
    }
  }

  alignedOffsetTable = table;
  alignedOffsetTableFont = fontData;
  alignedOffsetTableSize = totalGlyphs;
  return true;
}

void FontDecompressor::compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width,
                                          uint8_t height) {
  if (width == 0 || height == 0) return;
  const uint32_t rowStride = (width + 3) / 4;
  if (width % 4 == 0) {
    memcpy(packedDst, alignedSrc, rowStride * height);
    return;
  }
  uint8_t outByte = 0, outBits = 0;
  uint32_t writeIdx = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      outByte = (outByte << 2) | ((alignedSrc[y * rowStride + x / 4] >> ((3 - (x % 4)) * 2)) & 0x3);
      outBits += 2;
      if (outBits == 8) {
        packedDst[writeIdx++] = outByte;
        outByte = 0;
        outBits = 0;
      }
    }
  }
  if (outBits > 0) packedDst[writeIdx] = outByte << (8 - outBits);
}

// --- getBitmap: page buffer → hot group → decompress ---

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex) {
  const uint32_t tStart = micros();
  stats.getBitmapCalls++;

  if (!fontData->groups || fontData->groupCount == 0) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[glyph->dataOffset];
  }

  // Check page buffer slots (populated by prewarmCache — one slot per font style)
  for (uint8_t s = 0; s < pageSlotCount; s++) {
    const auto& slot = pageSlots[s];
    if (slot.fontData != fontData || slot.glyphCount == 0) continue;

    int left = 0, right = slot.glyphCount - 1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      if (slot.glyphs[mid].glyphIndex == glyphIndex) {
        if (slot.glyphs[mid].bufferOffset != UINT32_MAX) {
          stats.cacheHits++;
          stats.getBitmapTimeUs += micros() - tStart;
          return &slot.buffer[slot.glyphs[mid].bufferOffset];
        }
        break;  // Not extracted during prewarm; fall through to hot-group path
      }
      if (slot.glyphs[mid].glyphIndex < glyphIndex)
        left = mid + 1;
      else
        right = mid - 1;
    }
    break;  // Found the right slot but glyph wasn't in it; don't check other slots
  }

  // Fallback: hot group slot
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // Check if hot group already has this group decompressed — if not, decompress it
  if (!(hotGroup != nullptr && hotGroupFont == fontData && hotGroupIndex == groupIndex)) {
    stats.cacheMisses++;
    const EpdFontGroup& group = fontData->groups[groupIndex];

    // ensureCapacity may free the buffer, so the cached-group identity dies with it either way.
    hotGroupFont = nullptr;
    hotGroupIndex = UINT16_MAX;
    if (!ensureCapacity(hotGroup, hotGroupCapacity, group.uncompressedSize)) {
      LOG_ERR("FDC", "Failed to allocate %u bytes for hot group %u", group.uncompressedSize, groupIndex);
      stats.bitmapAllocFailures++;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    if (!decompressGroup(fontData, groupIndex, hotGroup, group.uncompressedSize)) {
      stats.bitmapAllocFailures++;
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    hotGroupFont = fontData;
    hotGroupIndex = groupIndex;
    stats.hotGroupBytes = group.uncompressedSize;
  } else {
    stats.cacheHits++;
  }

  // Compact just the requested glyph from byte-aligned data into scratch buffer
  if (!ensureCapacity(hotGlyphBuf, hotGlyphBufCapacity, glyph->dataLength)) {
    LOG_ERR("FDC", "Failed to allocate %u bytes for glyph scratch", (unsigned)glyph->dataLength);
    stats.bitmapAllocFailures++;
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  uint32_t alignedOff;
  if (ensureAlignedOffsetTable(fontData) && glyphIndex < alignedOffsetTableSize) {
    alignedOff = alignedOffsetTable[glyphIndex];
  } else {
    // Table build failed (OOM) or glyphIndex is out of range for some reason -- fall
    // back to the direct per-call computation rather than reading garbage/OOB.
    alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
  }
  compactSingleGlyph(&hotGroup[alignedOff], hotGlyphBuf, glyph->width, glyph->height);
  stats.getBitmapTimeUs += micros() - tStart;
  return hotGlyphBuf;
}

// --- Prewarm: pre-decompress glyph bitmaps for a page of text ---

int32_t FontDecompressor::findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint) {
  const EpdUnicodeInterval* intervals = fontData->intervals;
  const int count = fontData->intervalCount;

  if (count == 0) return -1;

  // Binary search
  int left = 0;
  int right = count - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &intervals[mid];

    if (codepoint < interval->first) {
      right = mid - 1;
    } else if (codepoint > interval->last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval->offset + (codepoint - interval->first));
    }
  }

  return -1;
}

int FontDecompressor::prewarmCache(const EpdFontData* fontData, const char* utf8Text) {
  if (!fontData || !fontData->groups || !utf8Text) return 0;

  // Allocate the next available slot (caller must call freePageBuffer/clearCache to reset)
  if (pageSlotCount >= MAX_PAGE_SLOTS) {
    LOG_ERR("FDC", "All %u page buffer slots full, cannot prewarm fontData=%p", MAX_PAGE_SLOTS, (void*)fontData);
    return -1;
  }
  PageSlot& slot = pageSlots[pageSlotCount];

  // Step 1: Collect unique glyph indices needed for this page. Heap-allocated (not a
  // stack array): at the current MAX_PAGE_GLYPHS this would be 16KB, too large to put
  // on the task stack safely.
  auto neededGlyphsBuf = makeUniqueNoThrow<uint32_t[]>(MAX_PAGE_GLYPHS);
  if (!neededGlyphsBuf) {
    LOG_ERR("FDC", "OOM allocating %u-entry glyph scratch buffer", MAX_PAGE_GLYPHS);
    return -1;
  }
  uint32_t* neededGlyphs = neededGlyphsBuf.get();
  uint16_t glyphCount = 0;
  bool glyphCapWarned = false;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    int32_t glyphIdx = findGlyphIndex(fontData, cp);
    if (glyphIdx < 0) continue;

    // Deduplicate
    bool found = false;
    for (uint16_t i = 0; i < glyphCount; i++) {
      if (neededGlyphs[i] == static_cast<uint32_t>(glyphIdx)) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (glyphCount < MAX_PAGE_GLYPHS) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(glyphIdx);
      } else if (!glyphCapWarned) {
        LOG_DBG("FDC", "Glyph cap (%u) reached during prewarm; excess glyphs will use hot-group fallback",
                MAX_PAGE_GLYPHS);
        glyphCapWarned = true;
      }
    }
  }

  // Add ligature output glyphs: if both input codepoints of a ligature pair are
  // in the needed set, the output glyph will be queried during rendering.
  if (fontData->ligaturePairs && fontData->ligaturePairCount > 0) {
    for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount < MAX_PAGE_GLYPHS; li++) {
      uint32_t leftCp = fontData->ligaturePairs[li].pair >> 16;
      uint32_t rightCp = fontData->ligaturePairs[li].pair & 0xFFFF;

      int32_t leftIdx = findGlyphIndex(fontData, leftCp);
      int32_t rightIdx = findGlyphIndex(fontData, rightCp);
      if (leftIdx < 0 || rightIdx < 0) continue;

      // Check if both inputs are in neededGlyphs
      bool hasLeft = false, hasRight = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(leftIdx)) hasLeft = true;
        if (neededGlyphs[i] == static_cast<uint32_t>(rightIdx)) hasRight = true;
        if (hasLeft && hasRight) break;
      }
      if (!hasLeft || !hasRight) continue;

      int32_t outIdx = findGlyphIndex(fontData, fontData->ligaturePairs[li].ligatureCp);
      if (outIdx < 0) continue;

      // Deduplicate
      bool found = false;
      for (uint16_t i = 0; i < glyphCount; i++) {
        if (neededGlyphs[i] == static_cast<uint32_t>(outIdx)) {
          found = true;
          break;
        }
      }
      if (!found) {
        neededGlyphs[glyphCount++] = static_cast<uint32_t>(outIdx);
      }
    }
  }

  if (glyphCount == 0) return 0;

  // Step 2: Compute total buffer size and collect unique groups. Heap-allocated like
  // neededGlyphs above -- at MAX_PAGE_GROUPS this would be 1KB, on top of the 16KB
  // glyph buffer and the groupAlignedTracker buffer below.
  uint32_t totalBytes = 0;
  auto neededGroupsBuf = makeUniqueNoThrow<uint16_t[]>(MAX_PAGE_GROUPS);
  if (!neededGroupsBuf) {
    LOG_ERR("FDC", "OOM allocating %u-entry group scratch buffer", MAX_PAGE_GROUPS);
    return -1;
  }
  uint16_t* neededGroups = neededGroupsBuf.get();
  uint16_t groupCount = 0;
  bool groupCapWarned = false;

  for (uint16_t i = 0; i < glyphCount; i++) {
    totalBytes += fontData->glyph[neededGlyphs[i]].dataLength;
    uint16_t gi = getGroupIndex(fontData, neededGlyphs[i]);
    bool found = false;
    for (uint16_t j = 0; j < groupCount; j++) {
      if (neededGroups[j] == gi) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (groupCount < MAX_PAGE_GROUPS) {
        neededGroups[groupCount++] = gi;
      } else if (!groupCapWarned) {
        LOG_DBG("FDC", "Group cap (%u) reached during prewarm; some groups will use hot-group fallback",
                MAX_PAGE_GROUPS);
        groupCapWarned = true;
      }
    }
  }

  stats.uniqueGroupsAccessed = groupCount;

  // Step 3: Allocate page buffer and lookup table for this slot.
  // totalBytes can legitimately be 0 (e.g. a page of only blank/space glyphs with no
  // bitmap data) -- malloc(0) is implementation-defined and commonly returns nullptr,
  // which isn't an allocation failure in that case. Only skip the buffer malloc (and
  // its nullptr check) when there's nothing to store.
  slot.buffer = totalBytes > 0 ? static_cast<uint8_t*>(malloc(totalBytes)) : nullptr;
  slot.glyphs = static_cast<PageGlyphEntry*>(malloc(glyphCount * sizeof(PageGlyphEntry)));
  if ((totalBytes > 0 && !slot.buffer) || !slot.glyphs) {
    LOG_ERR("FDC", "Failed to allocate page buffer (%u bytes, %u glyphs)", totalBytes, glyphCount);
    free(slot.buffer);
    free(slot.glyphs);
    slot = {};
    return glyphCount;
  }
  stats.pageBufferBytes += totalBytes;
  stats.pageGlyphsBytes += glyphCount * sizeof(PageGlyphEntry);

  slot.fontData = fontData;
  slot.glyphCount = glyphCount;
  pageSlotCount++;

  // Initialize lookup entries (bufferOffset = UINT32_MAX means not yet extracted)
  for (uint16_t i = 0; i < glyphCount; i++) {
    slot.glyphs[i] = {neededGlyphs[i], UINT32_MAX, 0};
  }

  // Sort by glyphIndex for binary search in getBitmap()
  for (uint16_t i = 1; i < glyphCount; i++) {
    PageGlyphEntry key = slot.glyphs[i];
    int j = i - 1;
    while (j >= 0 && slot.glyphs[j].glyphIndex > key.glyphIndex) {
      slot.glyphs[j + 1] = slot.glyphs[j];
      j--;
    }
    slot.glyphs[j + 1] = key;
  }

  // Step 3b: Pre-scan to compute each needed glyph's byte-aligned offset within its group.
  // This avoids recomputing aligned offsets per group during extraction in step 4.
  // Heap-allocated like neededGroups above, zero-initialized to match the stack array's
  // prior value-initialization.
  auto groupAlignedTrackerBuf = makeUniqueNoThrow<uint32_t[]>(MAX_PAGE_GROUPS);
  if (!groupAlignedTrackerBuf) {
    LOG_ERR("FDC", "OOM allocating %u-entry group offset tracker", MAX_PAGE_GROUPS);
    return -1;
  }
  uint32_t* groupAlignedTracker = groupAlignedTrackerBuf.get();  // running byte-aligned offset per needed group

  if (fontData->glyphToGroup) {
    // Frequency-grouped: single O(totalGlyphs) pass through glyphToGroup
    const auto& lastInterval = fontData->intervals[fontData->intervalCount - 1];
    const uint32_t totalGlyphs = lastInterval.offset + (lastInterval.last - lastInterval.first + 1);

    for (uint32_t i = 0; i < totalGlyphs; i++) {
      const uint16_t gi = fontData->glyphToGroup[i];
      // Find this glyph's group position in neededGroups
      uint16_t gpPos = groupCount;
      for (uint16_t j = 0; j < groupCount; j++) {
        if (neededGroups[j] == gi) {
          gpPos = j;
          break;
        }
      }
      if (gpPos == groupCount) continue;  // not a needed group

      const EpdGlyph& glyph = fontData->glyph[i];

      // Binary search in sorted slot.glyphs to find if glyph i is needed
      int left = 0, right = (int)slot.glyphCount - 1;
      while (left <= right) {
        const int mid = left + (right - left) / 2;
        if (slot.glyphs[mid].glyphIndex == i) {
          slot.glyphs[mid].alignedOffset = groupAlignedTracker[gpPos];
          break;
        }
        if (slot.glyphs[mid].glyphIndex < i)
          left = mid + 1;
        else
          right = mid - 1;
      }

      if (glyph.width > 0 && glyph.height > 0) {
        groupAlignedTracker[gpPos] += ((glyph.width + 3) / 4) * glyph.height;
      }
    }
  } else {
    // Contiguous-group: iterate each needed group's glyphs directly
    for (uint16_t g = 0; g < groupCount; g++) {
      const EpdFontGroup& group = fontData->groups[neededGroups[g]];
      uint32_t alignedOff = 0;
      for (uint16_t j = 0; j < group.glyphCount; j++) {
        const uint32_t glyphI = group.firstGlyphIndex + j;
        const EpdGlyph& glyph = fontData->glyph[glyphI];

        int left = 0, right = (int)slot.glyphCount - 1;
        while (left <= right) {
          const int mid = left + (right - left) / 2;
          if (slot.glyphs[mid].glyphIndex == glyphI) {
            slot.glyphs[mid].alignedOffset = alignedOff;
            break;
          }
          if (slot.glyphs[mid].glyphIndex < glyphI)
            left = mid + 1;
          else
            right = mid - 1;
        }

        if (glyph.width > 0 && glyph.height > 0) {
          alignedOff += ((glyph.width + 3) / 4) * glyph.height;
        }
      }
    }
  }

  // Step 4: For each unique group, decompress to temp buffer and extract needed glyphs
  uint32_t writeOffset = 0;
  int missed = 0;

  for (uint16_t g = 0; g < groupCount; g++) {
    uint16_t groupIdx = neededGroups[g];
    const EpdFontGroup& group = fontData->groups[groupIdx];

    auto* tempBuf = static_cast<uint8_t*>(malloc(group.uncompressedSize));
    if (!tempBuf) {
      LOG_ERR("FDC", "Failed to allocate temp buffer (%u bytes) for group %u", group.uncompressedSize, groupIdx);
      missed++;
      continue;
    }
    if (group.uncompressedSize > stats.peakTempBytes) {
      stats.peakTempBytes = group.uncompressedSize;
    }

    if (!decompressGroup(fontData, groupIdx, tempBuf, group.uncompressedSize)) {
      free(tempBuf);
      missed++;
      continue;
    }

    // Extract needed glyphs directly from the byte-aligned temp buffer, compacting on the fly.
    // alignedOffset was pre-computed in step 3b — no full-group compact scan needed.
    for (uint16_t i = 0; i < slot.glyphCount; i++) {
      if (slot.glyphs[i].bufferOffset != UINT32_MAX) continue;  // already extracted
      if (getGroupIndex(fontData, slot.glyphs[i].glyphIndex) != groupIdx) continue;

      const EpdGlyph& glyph = fontData->glyph[slot.glyphs[i].glyphIndex];
      compactSingleGlyph(&tempBuf[slot.glyphs[i].alignedOffset], &slot.buffer[writeOffset], glyph.width, glyph.height);
      slot.glyphs[i].bufferOffset = writeOffset;
      writeOffset += glyph.dataLength;
    }

    free(tempBuf);
  }

  LOG_DBG("FDC", "Prewarm: %u glyphs in %u bytes from %u groups (%d missed)", glyphCount, writeOffset, groupCount,
          missed);

  return missed;
}

// --- Stats ---

void FontDecompressor::resetStats() { stats = Stats{}; }

void FontDecompressor::logStats(const char* label) {
  const uint32_t total = stats.cacheHits + stats.cacheMisses;
  LOG_DBG("FDC", "[%s] hits=%lu misses=%lu (%.1f%% hit rate)", label, stats.cacheHits, stats.cacheMisses,
          total > 0 ? 100.0f * stats.cacheHits / total : 0.0f);
  LOG_DBG("FDC", "[%s] decompress=%lums groups_accessed=%u", label, stats.decompressTimeMs, stats.uniqueGroupsAccessed);
  LOG_DBG("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu hotGroup=%lu peakTemp=%lu", label, stats.pageBufferBytes,
          stats.pageGlyphsBytes, stats.hotGroupBytes, stats.peakTempBytes);
  if (stats.getBitmapCalls > 0) {
    LOG_DBG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
            stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
  }
  resetStats();
}
