#include "ArabicCharacter.h"

namespace ArabicShaper {

bool isArabicDiacritic(uint32_t cp) { return (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670; }

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
