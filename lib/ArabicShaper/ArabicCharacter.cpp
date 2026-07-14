#include "ArabicCharacter.h"

namespace ArabicShaper {

bool isArabicDiacritic(uint32_t cp) {
  // U+064B-065F/U+0670: standard tashkeel (fatha, damma, kasra, sukun, superscript alef, ...).
  // The remaining ranges are Quranic-specific combining marks (Unicode "Quranic Annotation
  // Signs" + the small high marks used for Uthmani-orthography sukun/waqf guidance) --
  // all zero-width, non-joining by Unicode's Joining_Type=Transparent, same as tashkeel.
  // Confirmed needed on a real device: KFGQPC Uthmanic Hafs Quran text uses U+06E1 (small
  // high dotless head of khah, a sukun variant) between letters that must otherwise join
  // normally (e.g. "ٱلۡمُفۡلِحُونَ") -- treating it as an unrecognized base letter instead
  // of a transparent mark broke the join and inserted a large gap where the two letters
  // should have connected.
  return (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 ||
         (cp >= 0x0610 && cp <= 0x061A) ||  // Quranic marks (sallallahou alayhe wassallam, etc.)
         (cp >= 0x06D6 && cp <= 0x06DC) ||  // Quranic annotation signs (small high ligatures)
         (cp >= 0x06DF && cp <= 0x06E4) ||  // small high marks (rounded zero, madda, ...)
         (cp >= 0x06E7 && cp <= 0x06E8) ||  // small high yeh / noon
         (cp >= 0x06EA && cp <= 0x06ED);    // empty centre marks, small low meem
}

// Extended-Arabic letters (Persian, Ottoman Turkish, Kurdish) that participate in
// shaping. Kept as an explicit list because the 0x066x-0x06Fx range is mostly
// digits, marks, and letters this shaper has no presentation forms for -- every
// codepoint here has a matching row in ARABIC_FORMS (see ArabicShapingTables.h).
static bool isExtendedArabicLetter(uint32_t cp) {
  switch (cp) {
    case 0x067E:  // Peh (Persian پ)
    case 0x0686:  // Tcheh (Persian چ)
    case 0x0698:  // Jeh (Persian ژ)
    case 0x06A9:  // Keheh (Persian ک)
    case 0x06AD:  // Ng (Ottoman ڭ)
    case 0x06AF:  // Gaf (Persian گ)
    case 0x06C0:  // Heh with Yeh Above (Persian ۀ)
    case 0x06CC:  // Farsi Yeh (Persian ی -- dotless at word end by design)
    case 0x06D5:  // Ae (Ottoman/Kurdish ە -- never joins forward)
      return true;
    default:
      return false;
  }
}

bool isArabicBaseChar(uint32_t cp) {
  return (cp >= 0x0621 && cp <= 0x064A && !isArabicDiacritic(cp)) || isExtendedArabicLetter(cp);
}

JoiningType getJoiningType(uint32_t cp) {
  if (isArabicDiacritic(cp)) return JoiningType::TRANSPARENT;

  // Right-joining only characters
  switch (cp) {
    case 0x0622:  // Alef with Madda
    case 0x0623:  // Alef with Hamza Above
    case 0x0624:  // Waw with Hamza Above
    case 0x0625:  // Alef with Hamza Below
    case 0x0627:  // Alef
    case 0x0629:  // Teh Marbuta
    case 0x062F:  // Dal
    case 0x0630:  // Thal
    case 0x0631:  // Ra
    case 0x0632:  // Zain
    case 0x0648:  // Waw
    case 0x0671:  // Alef Wasla (Quranic Uthmani orthography -- elidable/connecting hamza,
                  // e.g. the definite article "ٱلْ" written with Wasla instead of plain Alef).
                  // Outside isArabicBaseChar's 0x0621-0x064A range, so it fell through to
                  // NON_JOINING (like bare Hamza) without this case -- confirmed on a real
                  // device: "بِٱلۡغَيۡبِ" rendered with Beh disconnected from Alef Wasla,
                  // a visible gap where the two letters should have joined.
    case 0x0698:  // Jeh (Persian -- Reh-shaped)
    case 0x06C0:  // Heh with Yeh Above (Persian)
    case 0x06D5:  // Ae (Ottoman/Kurdish -- "non-joining to the following letter")
    // Lam-Alef ligatures (Presentation Forms-B) - right-joining
    case 0xFEF5:  // Lam+Alef Madda isolated
    case 0xFEF6:  // Lam+Alef Madda final
    case 0xFEF7:  // Lam+Alef Hamza Above isolated
    case 0xFEF8:  // Lam+Alef Hamza Above final
    case 0xFEF9:  // Lam+Alef Hamza Below isolated
    case 0xFEFA:  // Lam+Alef Hamza Below final
    case 0xFEFB:  // Lam+Alef isolated
    case 0xFEFC:  // Lam+Alef final
      return JoiningType::RIGHT_JOINING;
    default:
      break;
  }

  // Dual-joining Arabic letters (incl. the dual-joining extended letters --
  // the right-joining ones were already handled above)
  if (isArabicBaseChar(cp) && cp != 0x0621) {  // Hamza is non-joining
    return JoiningType::DUAL_JOINING;
  }

  // Hamza (isolated only)
  if (cp == 0x0621) return JoiningType::NON_JOINING;

  return JoiningType::NON_JOINING;
}

}  // namespace ArabicShaper
