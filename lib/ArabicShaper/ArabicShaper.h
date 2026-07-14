#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "ArabicCharacter.h"
#include "ArabicShapingTables.h"

namespace ArabicShaper {

// Shape Arabic text: apply contextual forms and Lam-Alef ligatures.
// Input: logical order UTF-8 string
// Output: visual order (reversed) shaped codepoints ready for left-to-right rendering
//
// hasGlyph: optional query for whether the active font actually has a usable glyph
// for a given codepoint. When provided, the Allah (U+FDF2) and Lam-Alef ligatures
// are only emitted if hasGlyph confirms the font has that single ligature glyph --
// our renderer draws one codepoint as exactly one glyph, so a font that instead
// joins those letters normally (no dedicated ligature glyph) needs the constituent
// letters shaped individually, or the whole word silently disappears (confirmed on
// a real device: KFGQPC Uthmanic Hafs has no U+FDF2, and "الله" vanished entirely
// because the old unconditional substitution had nothing to fall back to). Default
// (no callback) always applies ligatures, matching every font that shipped before
// this parameter existed.
std::vector<uint32_t> shapeText(const char* text, const std::function<bool(uint32_t)>& hasGlyph = nullptr);

// Get the contextual form for a base Arabic codepoint.
// prevJoins: whether the previous character can join to this one
// nextJoins: whether the next character can join from this one
uint32_t getContextualForm(uint32_t cp, bool prevJoins, bool nextJoins);

// Check for Lam-Alef ligature. Returns ligature codepoint or 0 if not a ligature pair.
// prevJoins: whether the character before Lam can join
uint32_t getLamAlefLigature(uint32_t alef, bool prevJoins);

// Cheap layout-time query: does `text` contain at least one legal kashida (tatweel)
// insertion point -- i.e. two adjacent letters whose joining types actually connect,
// so a stretched stroke can be spliced between them? Ligature-aware, via the same
// `hasGlyph` gating shapeText() uses. Used to decide, per word, whether it can absorb
// any of a justified line's spare width via kashida before falling back to widening
// inter-word gaps.
bool hasKashidaPoint(const char* text, const std::function<bool(uint32_t)>& hasGlyph = nullptr);

// Shape text exactly like shapeText(), but first splice literal U+0640 TATWEEL
// codepoints into the logical-order sequence at legal kashida points to add
// approximately `extraWidthPx` of rendered width, given the active font's fixed
// per-glyph `tatweelAdvancePx`. Whole tatweel glyphs are distributed evenly across
// every valid point in the word (remainder glyphs go to the earliest points), each
// insertion landing after any diacritics trailing the preceding base letter so mark
// positioning is undisturbed. Returns VISUAL order exactly like shapeText() -- the
// same per-glyph draw/measure loop works for both. Degrades to shapeText()'s own
// output when the word has no valid point, `extraWidthPx` is smaller than one
// glyph's width, or `hasGlyph` reports the font has no U+0640 glyph.
std::vector<uint32_t> shapeTextWithKashida(const char* text, int extraWidthPx, int tatweelAdvancePx,
                                            const std::function<bool(uint32_t)>& hasGlyph = nullptr);

}  // namespace ArabicShaper
