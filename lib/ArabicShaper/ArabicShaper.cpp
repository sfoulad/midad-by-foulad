#include "ArabicShaper.h"

#include <algorithm>

// Inline UTF-8 decode to avoid header dependency
static uint32_t decodeUtf8(const unsigned char** ptr) {
  if (**ptr == 0) return 0;
  unsigned char c = **ptr;
  uint32_t cp;
  int bytes;
  if (c < 0x80) {
    cp = c;
    bytes = 1;
  } else if ((c >> 5) == 0x6) {
    cp = c & 0x1F;
    bytes = 2;
  } else if ((c >> 4) == 0xE) {
    cp = c & 0x0F;
    bytes = 3;
  } else if ((c >> 3) == 0x1E) {
    cp = c & 0x07;
    bytes = 4;
  } else {
    (*ptr)++;
    return 0xFFFD;
  }
  for (int i = 1; i < bytes; i++) {
    unsigned char cont = (*ptr)[i];
    if ((cont & 0xC0) != 0x80) {
      *ptr += i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (cont & 0x3F);
  }
  *ptr += bytes;
  return cp;
}

namespace ArabicShaper {

static const ArabicFormEntry* findFormEntry(uint32_t cp) {
  for (size_t i = 0; i < ARABIC_FORMS_COUNT; i++) {
    if (ARABIC_FORMS[i].base == cp) return &ARABIC_FORMS[i];
  }
  return nullptr;
}

uint32_t getContextualForm(uint32_t cp, bool prevJoins, bool nextJoins, const std::function<bool(uint32_t)>& hasGlyph) {
  const ArabicFormEntry* entry = findFormEntry(cp);
  if (!entry) return cp;

  const auto avail = [&](uint32_t formCp) { return formCp != 0 && (!hasGlyph || hasGlyph(formCp)); };

  if (prevJoins && nextJoins && avail(entry->medial)) return entry->medial;
  if (prevJoins && avail(entry->final_)) return entry->final_;
  if (nextJoins && avail(entry->initial)) return entry->initial;
  if (avail(entry->isolated)) return entry->isolated;
  return cp;
}

uint32_t getLamAlefLigature(uint32_t alef, bool prevJoins) {
  for (size_t i = 0; i < LAM_ALEF_COUNT; i++) {
    if (LAM_ALEF_LIGATURES[i].alef == alef) {
      return prevJoins ? LAM_ALEF_LIGATURES[i].final_ : LAM_ALEF_LIGATURES[i].isolated;
    }
  }
  return 0;
}

// Check if a joining type can join to the left (has a connection on its left side)
static bool joinsToLeft(JoiningType jt) { return jt == JoiningType::DUAL_JOINING; }

// Check if a joining type can join to the right (has a connection on its right side)
static bool joinsToRight(JoiningType jt) { return jt == JoiningType::DUAL_JOINING || jt == JoiningType::RIGHT_JOINING; }

// Unicode Bidi_Mirrored property (subset): characters whose glyph must be swapped
// when they end up in a resolved RTL context. Only pairs actually seen in books are
// included to keep the table compact; curly quotes (U+201C..U+201D, U+2018..U+2019)
// are intentionally absent because Unicode marks them Bidi_Mirrored=No.
static uint32_t bidiMirror(uint32_t cp) {
  switch (cp) {
    case 0x0028:
      return 0x0029;  // ( -> )
    case 0x0029:
      return 0x0028;  // ) -> (
    case 0x005B:
      return 0x005D;  // [ -> ]
    case 0x005D:
      return 0x005B;  // ] -> [
    case 0x007B:
      return 0x007D;  // { -> }
    case 0x007D:
      return 0x007B;  // } -> {
    case 0x003C:
      return 0x003E;  // < -> >
    case 0x003E:
      return 0x003C;  // > -> <
    case 0x00AB:
      return 0x00BB;  // « -> »
    case 0x00BB:
      return 0x00AB;  // » -> «
    case 0x2039:
      return 0x203A;  // ‹ -> ›
    case 0x203A:
      return 0x2039;  // › -> ‹
    default:
      return cp;
  }
}

// Steps 1, 1.5, 2: decode UTF-8, resolve the "الله" (Allah) ligature, resolve
// Lam-Alef ligatures. Returns logical-order codepoints, pre-contextual-shaping.
// Split out of shapeText() so hasKashidaPoint()/shapeTextWithKashida() can reuse
// it without re-running (or duplicating) the rest of the pipeline.
static std::vector<uint32_t> resolveLigatures(const char* text, const std::function<bool(uint32_t)>& hasGlyph) {
  if (!text || !*text) return {};

  // Step 1: Decode UTF-8 to codepoints
  std::vector<uint32_t> codepoints;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text);
  uint32_t cp;
  while ((cp = decodeUtf8(&ptr))) {
    codepoints.push_back(cp);
  }

  if (codepoints.empty()) return {};

  // Step 1.5: Detect the "الله" (Allah) ligature -- Alef, Lam, Lam, Heh, skipping any
  // diacritics between letters (they'd be dropped in step 3 anyway). Unicode defines
  // only one presentation form for this specific ligature (U+FDF2), and it's
  // "isolated" -- no connection point on either side. It only applies when nothing
  // connects into the Alef from before (e.g. "بالله" keeps the Beh joined normally,
  // no ligature) and nothing connects out of the Heh afterward (e.g. "اللهجة",
  // "اللهم" are different words entirely, not "Allah" + a suffix). Matches
  // arabic_reshaper's behavior exactly (verified against a real 30k-word book). The
  // bundled Noto Sans Arabic font has this glyph via its own GSUB rules, but
  // fontconvert.py's generic 2-glyph ligature extractor can't emit a 4-letter chain,
  // so it's special-cased here instead.
  {
    std::vector<uint32_t> afterAllah;
    afterAllah.reserve(codepoints.size());
    size_t i = 0;
    while (i < codepoints.size()) {
      if (codepoints[i] == 0x0627) {  // Alef
        size_t j = i + 1;
        auto skipDiacritics = [&]() {
          while (j < codepoints.size() && isArabicDiacritic(codepoints[j])) j++;
        };
        skipDiacritics();
        bool isAllahSequence = false;
        if (j < codepoints.size() && codepoints[j] == 0x0644) {  // Lam
          j++;
          skipDiacritics();
          if (j < codepoints.size() && codepoints[j] == 0x0644) {  // Lam
            j++;
            skipDiacritics();
            if (j < codepoints.size() && codepoints[j] == 0x0647) {  // Heh
              isAllahSequence = true;
            }
          }
        }

        if (isAllahSequence) {
          bool prevConnects = false;
          for (int p = static_cast<int>(afterAllah.size()) - 1; p >= 0; p--) {
            JoiningType pjt = getJoiningType(afterAllah[p]);
            if (pjt == JoiningType::TRANSPARENT) continue;
            prevConnects = joinsToLeft(pjt);
            break;
          }
          size_t afterHeh = j + 1;
          while (afterHeh < codepoints.size() && isArabicDiacritic(codepoints[afterHeh])) afterHeh++;
          const bool nextExists = afterHeh < codepoints.size() && isArabicBaseChar(codepoints[afterHeh]);

          if (!prevConnects && !nextExists && (!hasGlyph || hasGlyph(0xFDF2))) {
            afterAllah.push_back(0xFDF2);
            i = afterHeh;
            continue;
          }
        }
      }
      afterAllah.push_back(codepoints[i]);
      i++;
    }
    codepoints.swap(afterAllah);
  }

  // Step 2: Apply Lam-Alef ligatures
  std::vector<uint32_t> afterLigatures;
  afterLigatures.reserve(codepoints.size());
  for (size_t i = 0; i < codepoints.size(); i++) {
    if (codepoints[i] == 0x0644 && i + 1 < codepoints.size()) {  // Lam
      // Look ahead past any diacritics to find the Alef
      size_t alefIdx = i + 1;
      while (alefIdx < codepoints.size() && isArabicDiacritic(codepoints[alefIdx])) {
        alefIdx++;
      }
      if (alefIdx < codepoints.size()) {
        // Check if prev char joins to Lam
        bool prevJoins = false;
        for (int p = static_cast<int>(afterLigatures.size()) - 1; p >= 0; p--) {
          JoiningType pjt = getJoiningType(afterLigatures[p]);
          if (pjt == JoiningType::TRANSPARENT) continue;
          prevJoins = joinsToLeft(pjt);
          break;
        }
        uint32_t lig = getLamAlefLigature(codepoints[alefIdx], prevJoins);
        if (lig != 0 && (!hasGlyph || hasGlyph(lig))) {
          afterLigatures.push_back(lig);
          // Copy diacritics between Lam and Alef
          for (size_t d = i + 1; d < alefIdx; d++) {
            afterLigatures.push_back(codepoints[d]);
          }
          i = alefIdx;  // Skip past the Alef
          continue;
        }
      }
    }
    afterLigatures.push_back(codepoints[i]);
  }

  return afterLigatures;
}

// Step 3: apply contextual forms (isolated/initial/medial/final) to a logical-order
// codepoint sequence (the output of resolveLigatures(), or that output with kashida
// tatweel codepoints spliced in -- U+0640 is DUAL_JOINING and has no dedicated
// contextual-form row, so it passes through unchanged in every position, which is
// exactly the correct visual behavior for a straight joining stroke).
static std::vector<uint32_t> applyContextualForms(const std::vector<uint32_t>& afterLigatures,
                                                  const std::function<bool(uint32_t)>& hasGlyph) {
  std::vector<uint32_t> shaped;
  shaped.reserve(afterLigatures.size());

  for (size_t i = 0; i < afterLigatures.size(); i++) {
    uint32_t c = afterLigatures[i];

    // Tashkeel/harakat pass through as zero-advance combining marks. A mark is
    // TRANSPARENT for joining (the prev/next scans skip it), sits right after
    // its base letter logically, and the RTL run reversal in step 4 therefore
    // places it immediately BEFORE the base in visual order -- drawArabicText
    // then draws it at the base's pen position (marks carry ~zero advance) and
    // the font's static mark offsets stack it above/below the letter. Because
    // the advance is zero, layout/pagination is unchanged by vocalization.
    // (These were previously dropped outright, matching arabic_reshaper's
    // delete_harakat=true default -- acceptable for mostly-unvocalized novels,
    // catastrophic for the fully-vocalized Quran.)
    if (isArabicDiacritic(c)) {
      shaped.push_back(c);
      continue;
    }

    // Skip non-Arabic and already-shaped (ligature) codepoints
    if (!isArabicBaseChar(c)) {
      shaped.push_back(c);
      continue;
    }

    // Find previous non-transparent joining type
    bool prevJoins = false;
    for (int p = static_cast<int>(i) - 1; p >= 0; p--) {
      JoiningType pjt = getJoiningType(afterLigatures[p]);
      if (pjt == JoiningType::TRANSPARENT) continue;
      prevJoins = joinsToLeft(pjt);
      break;
    }

    // Find next non-transparent joining type
    bool nextJoins = false;
    for (size_t n = i + 1; n < afterLigatures.size(); n++) {
      JoiningType njt = getJoiningType(afterLigatures[n]);
      if (njt == JoiningType::TRANSPARENT) continue;
      nextJoins = joinsToRight(njt);
      break;
    }

    shaped.push_back(getContextualForm(c, prevJoins, nextJoins, hasGlyph));
  }

  return shaped;
}

// Step 4: Simplified BiDi reordering for visual order.
static std::vector<uint32_t> reorderVisual(const std::vector<uint32_t>& shaped) {
  // Classify each codepoint as RTL, LTR, or NEUTRAL
  enum class BidiDir : uint8_t { LTR, RTL, NEUTRAL };
  const size_t len = shaped.size();
  std::vector<BidiDir> dirs(len);

  // Arabic-Indic / Extended Arabic-Indic digits (AN), tracked separately from western
  // 0-9 digits (EN). UAX#9 rule W4 merges a single separator between two EN runs into
  // that same EN run (so "1990-2020" stays as typed), but that rule is EN-specific --
  // it does not apply to AN. Per rule N1, AN acts like R for neutral resolution, so a
  // hyphen between two AN digit runs (e.g. a footnote year range "١٨٧٩-١٩٧٠") resolves
  // RTL instead, and the two runs swap visual position. Confirmed against
  // arabic_reshaper + python-bidi on a real book (see ArabicShaper tests).
  std::vector<bool> isArabicIndicDigit(len, false);
  for (size_t i = 0; i < len; i++) {
    uint32_t c = shaped[i];
    // Digits (EN + AN) — European, Arabic-Indic, Extended Arabic-Indic (Persian/Urdu).
    // Checked before the Arabic range so AN digits (U+0660-0669) don't get swept into RTL.
    if ((c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9)) {
      dirs[i] = BidiDir::LTR;
      isArabicIndicDigit[i] = c >= 0x0660;
    } else if ((c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) || (c >= 0xFB50 && c <= 0xFDFF) ||
               (c >= 0xFE70 && c <= 0xFEFF)) {
      dirs[i] = BidiDir::RTL;
    } else if (c <= 0x20 || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '<' ||
               c == '>' || c == ',' || c == '.' || c == ':' || c == ';' || c == '-' || c == '!' || c == '?' ||
               c == '/' || c == '\'' || c == '"' || c == 0x00AB || c == 0x00BB || c == 0x2039 || c == 0x203A) {
      dirs[i] = BidiDir::NEUTRAL;
    } else {
      dirs[i] = BidiDir::LTR;
    }
  }

  // Resolve neutrals: brackets use content direction, others use neighbor context
  for (size_t i = 0; i < len; i++) {
    if (dirs[i] != BidiDir::NEUTRAL) continue;
    uint32_t c = shaped[i];

    if (c == '(' || c == '[') {
      // Opening bracket: look right for first strong char
      BidiDir found = BidiDir::RTL;  // fallback to base direction
      for (size_t j = i + 1; j < len; j++) {
        if (dirs[j] == BidiDir::LTR) {
          found = BidiDir::LTR;
          break;
        }
        if (dirs[j] == BidiDir::RTL) {
          found = BidiDir::RTL;
          break;
        }
      }
      dirs[i] = found;
    } else if (c == ')' || c == ']') {
      // Closing bracket: look left for first strong char
      BidiDir found = BidiDir::RTL;
      for (int j = static_cast<int>(i) - 1; j >= 0; j--) {
        if (dirs[j] == BidiDir::LTR) {
          found = BidiDir::LTR;
          break;
        }
        if (dirs[j] == BidiDir::RTL) {
          found = BidiDir::RTL;
          break;
        }
      }
      dirs[i] = found;
    } else {
      // Other neutrals: if both neighbors agree, use that; else base direction (RTL)
      BidiDir left = BidiDir::RTL, right = BidiDir::RTL;
      int leftIdx = -1;
      size_t rightIdx = len;
      for (int j = static_cast<int>(i) - 1; j >= 0; j--) {
        if (dirs[j] != BidiDir::NEUTRAL) {
          left = dirs[j];
          leftIdx = j;
          break;
        }
      }
      for (size_t j = i + 1; j < len; j++) {
        if (dirs[j] != BidiDir::NEUTRAL) {
          right = dirs[j];
          rightIdx = j;
          break;
        }
      }
      if (c == '-' && leftIdx >= 0 && rightIdx < len && left == BidiDir::LTR && right == BidiDir::LTR &&
          isArabicIndicDigit[leftIdx] && isArabicIndicDigit[rightIdx]) {
        // A hyphen-minus directly between two Arabic-Indic digit runs (e.g. a year
        // range) isn't absorbed by W4 (that rule's ES-merge clause is EN-only) and
        // resolves as RTL per N1, swapping the two runs' order. A period or comma
        // (CS) between two numbers of the same type DOES merge per W4's second
        // clause -- e.g. a decimal "٤٣.٥" (43.5) must stay together -- so this only
        // narrows to '-' specifically, not every neutral.
        dirs[i] = BidiDir::RTL;
      } else {
        dirs[i] = (left == right) ? left : BidiDir::RTL;
      }
    }
  }

  // Build runs of consecutive same-direction chars
  struct Run {
    size_t start, end;  // [start, end)
    BidiDir dir;
  };
  std::vector<Run> runs;
  if (len > 0) {
    size_t runStart = 0;
    for (size_t i = 1; i <= len; i++) {
      if (i == len || dirs[i] != dirs[runStart]) {
        runs.push_back({runStart, i, dirs[runStart]});
        runStart = i;
      }
    }
  }

  // Build visual order: reverse overall run order (RTL base), reverse chars within RTL runs
  std::vector<uint32_t> visual;
  visual.reserve(len);
  for (int r = static_cast<int>(runs.size()) - 1; r >= 0; r--) {
    if (runs[r].dir == BidiDir::RTL) {
      // RTL run: reverse chars (logical RTL → visual LTR) and apply Bidi_Mirrored
      // to characters like ( ) [ ] « » resolved to RTL context
      for (int i = static_cast<int>(runs[r].end) - 1; i >= static_cast<int>(runs[r].start); i--) {
        visual.push_back(bidiMirror(shaped[i]));
      }
    } else {
      // LTR run: keep char order
      for (size_t i = runs[r].start; i < runs[r].end; i++) {
        visual.push_back(shaped[i]);
      }
    }
  }

  return visual;
}

std::vector<uint32_t> shapeText(const char* text, const std::function<bool(uint32_t)>& hasGlyph) {
  std::vector<uint32_t> logical = resolveLigatures(text, hasGlyph);
  if (logical.empty()) return {};
  return reorderVisual(applyContextualForms(logical, hasGlyph));
}

// Find every legal kashida (tatweel) insertion point in a logical-order codepoint
// sequence -- points[i] == true means a kashida run may be inserted immediately
// before index i. A point exists between two adjacent base letters (skipping any
// TRANSPARENT diacritics between them, which is what keeps an inserted run on the
// correct side of a trailing harakat mark -- the point is always keyed to the
// position right before the NEXT base letter, never between a letter and its own
// mark) when the earlier letter's joining type extends forward (DUAL_JOINING) and
// the later letter's joining type accepts a connection from before
// (DUAL_JOINING or RIGHT_JOINING). Ligature codepoints (e.g. U+FDF2 "Allah", the
// Lam-Alef presentation forms) fall out of this correctly without special-casing:
// getJoiningType() classifies U+FDF2 as isolated-only (NON_JOINING) and the
// Lam-Alef forms as RIGHT_JOINING, so a word that collapsed into one of those
// ligatures during resolveLigatures() automatically loses the internal points it
// no longer has, since there's no longer a separate codepoint there to attach one.
static std::vector<bool> computeKashidaPoints(const std::vector<uint32_t>& logical) {
  std::vector<bool> points(logical.size(), false);
  int lastBaseIdx = -1;
  for (size_t i = 0; i < logical.size(); i++) {
    JoiningType jt = getJoiningType(logical[i]);
    if (jt == JoiningType::TRANSPARENT) continue;  // diacritic: doesn't break the adjacency search

    if (lastBaseIdx >= 0) {
      JoiningType prevType = getJoiningType(logical[static_cast<size_t>(lastBaseIdx)]);
      if (joinsToLeft(prevType) && joinsToRight(jt)) {
        points[i] = true;
      }
    }
    lastBaseIdx = static_cast<int>(i);
  }
  return points;
}

bool hasKashidaPoint(const char* text, const std::function<bool(uint32_t)>& hasGlyph) {
  std::vector<uint32_t> logical = resolveLigatures(text, hasGlyph);
  if (logical.empty()) return false;
  for (bool p : computeKashidaPoints(logical)) {
    if (p) return true;
  }
  return false;
}

std::vector<uint32_t> shapeTextWithKashida(const char* text, int extraWidthPx, int tatweelAdvancePx,
                                           const std::function<bool(uint32_t)>& hasGlyph) {
  if (extraWidthPx <= 0 || tatweelAdvancePx <= 0 || (hasGlyph && !hasGlyph(0x0640))) {
    return shapeText(text, hasGlyph);
  }

  std::vector<uint32_t> logical = resolveLigatures(text, hasGlyph);
  if (logical.empty()) return {};

  const std::vector<bool> points = computeKashidaPoints(logical);
  const int numPoints = static_cast<int>(std::count(points.begin(), points.end(), true));
  const int totalTatweels = extraWidthPx / tatweelAdvancePx;
  if (numPoints == 0 || totalTatweels <= 0) {
    return reorderVisual(applyContextualForms(logical, hasGlyph));
  }

  // Distribute whole tatweel glyphs evenly across every valid point; any remainder
  // (extraWidthPx not an exact multiple of numPoints * tatweelAdvancePx) goes to the
  // earliest points, one extra glyph each -- same "remainder to the front" rounding
  // convention as ParsedText's inter-word justify-extra distribution.
  const int perPoint = totalTatweels / numPoints;
  const int remainder = totalTatweels % numPoints;

  std::vector<uint32_t> augmented;
  augmented.reserve(logical.size() + static_cast<size_t>(totalTatweels));
  int pointsSeen = 0;
  for (size_t i = 0; i < logical.size(); i++) {
    if (points[i]) {
      const int count = perPoint + (pointsSeen < remainder ? 1 : 0);
      for (int t = 0; t < count; t++) augmented.push_back(0x0640);
      pointsSeen++;
    }
    augmented.push_back(logical[i]);
  }

  return reorderVisual(applyContextualForms(augmented, hasGlyph));
}

}  // namespace ArabicShaper
