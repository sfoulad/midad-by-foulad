#pragma once
#include <cstdint>

namespace ArabicShaper {

struct ArabicFormEntry {
  uint16_t base;
  uint16_t isolated;
  uint16_t final_;
  uint16_t initial;
  uint16_t medial;
};

// Arabic Presentation Forms-B lookup table
// Maps base Arabic characters to their contextual forms
static const ArabicFormEntry ARABIC_FORMS[] = {
    // Base     Isolated  Final    Initial  Medial
    {0x0621, 0xFE80, 0x0000, 0x0000, 0x0000},  // Hamza (no joining forms)
    {0x0622, 0xFE81, 0xFE82, 0x0000, 0x0000},  // Alef Madda
    {0x0623, 0xFE83, 0xFE84, 0x0000, 0x0000},  // Alef Hamza Above
    {0x0624, 0xFE85, 0xFE86, 0x0000, 0x0000},  // Waw Hamza Above
    {0x0625, 0xFE87, 0xFE88, 0x0000, 0x0000},  // Alef Hamza Below
    {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},  // Yeh Hamza Above
    {0x0627, 0xFE8D, 0xFE8E, 0x0000, 0x0000},  // Alef
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92},  // Beh
    {0x0629, 0xFE93, 0xFE94, 0x0000, 0x0000},  // Teh Marbuta
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98},  // Teh
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C},  // Theh
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},  // Jeem
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4},  // Hah
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8},  // Khah
    {0x062F, 0xFEA9, 0xFEAA, 0x0000, 0x0000},  // Dal
    {0x0630, 0xFEAB, 0xFEAC, 0x0000, 0x0000},  // Thal
    {0x0631, 0xFEAD, 0xFEAE, 0x0000, 0x0000},  // Reh
    {0x0632, 0xFEAF, 0xFEB0, 0x0000, 0x0000},  // Zain
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4},  // Seen
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8},  // Sheen
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},  // Sad
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0},  // Dad
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4},  // Tah
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},  // Zah
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC},  // Ain
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0},  // Ghain
    {0x0640, 0x0640, 0x0640, 0x0640, 0x0640},  // Tatweel (kashida)
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4},  // Feh
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8},  // Qaf
    {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC},  // Kaf
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},  // Lam
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4},  // Meem
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8},  // Noon
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC},  // Heh
    {0x0648, 0xFEED, 0xFEEE, 0x0000, 0x0000},  // Waw
    // Alef Maksura is dual-joining per Unicode, but its true initial/medial
    // presentation forms (U+FBE8/U+FBE9, dotless) don't exist in the bundled fonts.
    // Mid-word ALEF MAKSURA only occurs in real text when the author typed it in
    // place of YEH (the common Egyptian keyboard convention, e.g. "التركىز"), so
    // joining positions render with the YEH initial/medial forms -- exactly the
    // letter the reader expects there. Word-final/isolated positions (where genuine
    // alef maksura actually occurs: على, إلى) keep the correct dotless forms.
    {0x0649, 0xFEEF, 0xFEF0, 0xFEF3, 0xFEF4},  // Alef Maksura
    {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},  // Yeh
    // Extended-Arabic letters (Persian / Ottoman Turkish / Kurdish), shaped via
    // Arabic Presentation Forms-A. Every base listed here must also be in
    // isExtendedArabicLetter() (ArabicCharacter.cpp) so it classifies as a
    // shapeable base char with the right joining type, and its form codepoints
    // must be in the bundled font intervals (fontconvert.py --script arabic).
    {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59},  // Peh
    {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D},  // Tcheh
    {0x0698, 0xFB8A, 0xFB8B, 0x0000, 0x0000},  // Jeh -- Reh-shaped, right-joining
    {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91},  // Keheh
    {0x06AD, 0xFBD3, 0xFBD4, 0xFBD5, 0xFBD6},  // Ng
    {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95},  // Gaf
    {0x06C0, 0xFBA4, 0xFBA5, 0x0000, 0x0000},  // Heh with Yeh Above
    // Farsi Yeh: dotless in final/isolated position by design (the Persian
    // convention the Reddit request called out); FBFC-FBFF encode exactly that.
    {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF},  // Farsi Yeh
    // Ae: no presentation forms are encoded for it in Unicode. Isolated keeps the
    // base glyph; final borrows Heh's final form (U+FEEA) -- both are the same
    // dotless bowl shape, which is exactly how it renders in Ottoman/Kurdish text.
    {0x06D5, 0x06D5, 0xFEEA, 0x0000, 0x0000},  // Ae
};

static constexpr size_t ARABIC_FORMS_COUNT = sizeof(ARABIC_FORMS) / sizeof(ARABIC_FORMS[0]);

// Lam-Alef ligature table
struct LamAlefEntry {
  uint16_t alef;      // Alef variant
  uint16_t isolated;  // Ligature isolated form
  uint16_t final_;    // Ligature final form
};

static const LamAlefEntry LAM_ALEF_LIGATURES[] = {
    {0x0622, 0xFEF5, 0xFEF6},  // Lam + Alef Madda
    {0x0623, 0xFEF7, 0xFEF8},  // Lam + Alef Hamza Above
    {0x0625, 0xFEF9, 0xFEFA},  // Lam + Alef Hamza Below
    {0x0627, 0xFEFB, 0xFEFC},  // Lam + Alef
};

static constexpr size_t LAM_ALEF_COUNT = sizeof(LAM_ALEF_LIGATURES) / sizeof(LAM_ALEF_LIGATURES[0]);

}  // namespace ArabicShaper
