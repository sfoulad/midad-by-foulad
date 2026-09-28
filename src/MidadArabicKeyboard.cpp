#include "MidadArabicKeyboard.h"

#include <FreeInkUIGfxRenderer.h>

#include "activities/util/KeyboardExtension.h"

namespace fui = freeink::ui;

namespace midad_arabic_keyboard {
namespace {

// Key ids for this layer only. Kept well clear of the ASCII values the builtin
// layers use for their own keys and of the negative QWERTY_KEY_* specials, so a
// value can never be resolved against the wrong layer.
constexpr int16_t ARABIC_KEY_BASE = 3100;

#define AK(label, output, value) \
  fui::KeyboardKey { label, output, fui::KeyKind::Normal, fui::StateNormal, value, 1, true, nullptr }
#define AKW(label, output, value, units) \
  fui::KeyboardKey { label, output, fui::KeyKind::Normal, fui::StateNormal, value, units, true, nullptr }
#define AKS(label, kind, value, units) \
  fui::KeyboardKey { label, nullptr, kind, fui::StateNormal, value, units, true, nullptr }

// The 28 letters plus the hamza carriers and the ta marbuta/alef maqsura a
// reader actually types. Carried over unchanged from Midad's previous keyboard:
// the catalog server folds hamza, alef maqsura and ta marbuta before matching,
// so someone typing plain ا still finds أ -- but this keyboard is not only for
// search, and a keyboard that cannot type إبراهيم is not an Arabic keyboard.
//
// No shift row: Arabic has no case, which is also why layer() ignores `shifted`.
const fui::KeyboardKey ARA_ROW1[] = {AK("ض", "ض", ARABIC_KEY_BASE + 0), AK("ص", "ص", ARABIC_KEY_BASE + 1),
                                     AK("ث", "ث", ARABIC_KEY_BASE + 2), AK("ق", "ق", ARABIC_KEY_BASE + 3),
                                     AK("ف", "ف", ARABIC_KEY_BASE + 4), AK("غ", "غ", ARABIC_KEY_BASE + 5),
                                     AK("ع", "ع", ARABIC_KEY_BASE + 6), AK("ه", "ه", ARABIC_KEY_BASE + 7),
                                     AK("خ", "خ", ARABIC_KEY_BASE + 8), AK("ح", "ح", ARABIC_KEY_BASE + 9)};
const fui::KeyboardKey ARA_ROW2[] = {AK("ج", "ج", ARABIC_KEY_BASE + 10), AK("د", "د", ARABIC_KEY_BASE + 11),
                                     AK("ش", "ش", ARABIC_KEY_BASE + 12), AK("س", "س", ARABIC_KEY_BASE + 13),
                                     AK("ي", "ي", ARABIC_KEY_BASE + 14), AK("ب", "ب", ARABIC_KEY_BASE + 15),
                                     AK("ل", "ل", ARABIC_KEY_BASE + 16), AK("ا", "ا", ARABIC_KEY_BASE + 17),
                                     AK("ت", "ت", ARABIC_KEY_BASE + 18), AK("ن", "ن", ARABIC_KEY_BASE + 19)};
const fui::KeyboardKey ARA_ROW3[] = {AK("م", "م", ARABIC_KEY_BASE + 20), AK("ك", "ك", ARABIC_KEY_BASE + 21),
                                     AK("ط", "ط", ARABIC_KEY_BASE + 22), AK("ئ", "ئ", ARABIC_KEY_BASE + 23),
                                     AK("ء", "ء", ARABIC_KEY_BASE + 24), AK("ؤ", "ؤ", ARABIC_KEY_BASE + 25),
                                     AK("ر", "ر", ARABIC_KEY_BASE + 26), AK("ى", "ى", ARABIC_KEY_BASE + 27),
                                     AK("ة", "ة", ARABIC_KEY_BASE + 28), AK("و", "و", ARABIC_KEY_BASE + 29)};
const fui::KeyboardKey ARA_ROW4[] = {AK("ز", "ز", ARABIC_KEY_BASE + 30), AK("ظ", "ظ", ARABIC_KEY_BASE + 31),
                                     AK("ذ", "ذ", ARABIC_KEY_BASE + 32), AK("لا", "لا", ARABIC_KEY_BASE + 33),
                                     AK("أ", "أ", ARABIC_KEY_BASE + 34), AK("إ", "إ", ARABIC_KEY_BASE + 35),
                                     AK("آ", "آ", ARABIC_KEY_BASE + 36), AK("،", "،", ARABIC_KEY_BASE + 37),
                                     AK("؟", "؟", ARABIC_KEY_BASE + 38), AK(".", ".", ARABIC_KEY_BASE + 39)};

// The extra layer owns its whole grid, including this action row -- the
// extension contract. Mode returns to the letter layer; the activity overrides
// the Mode label, so the one here is only a fallback.
const fui::KeyboardKey ARA_BOTTOM[] = {AKS("abc", fui::KeyKind::Mode, fui::QWERTY_KEY_MODE, 2),
                                       AKW(" ", " ", ARABIC_KEY_BASE + 40, 4),
                                       AKS("Del", fui::KeyKind::Delete, fui::QWERTY_KEY_BACKSPACE, 2),
                                       AKS("OK", fui::KeyKind::Ok, fui::QWERTY_KEY_ENTER, 2)};

#undef AK
#undef AKW
#undef AKS

const fui::KeyboardRow ARA_ROWS[] = {
    {ARA_ROW1, 10, 0}, {ARA_ROW2, 10, 0}, {ARA_ROW3, 10, 0}, {ARA_ROW4, 10, 0}, {ARA_BOTTOM, 4, 0}};

const fui::KeyboardLayout ARABIC_LAYOUT{ARA_ROWS, 5};

// Label drawn on the mode key at the point in the cycle where the next stop is
// this layer. Not translated: it names the script in the script itself, the same
// way the built-in "abc" names the Latin layer in Latin.
constexpr char ARABIC_LAYER_LABEL[] = "عربي";

bool gPreferNextEntry = false;

const fui::KeyboardLayout* layer(const bool shifted) {
  (void)shifted;  // Arabic is caseless -- the same grid serves both shift states.
  return &ARABIC_LAYOUT;
}

const char* label() { return ARABIC_LAYER_LABEL; }

bool consumePreference() {
  const bool preferred = gPreferNextEntry;
  gPreferNextEntry = false;
  return preferred;
}

}  // namespace

void install() {
  setKeyboardExtensionLayerProvider(&layer);
  setKeyboardExtensionLabelProvider(&label);
  setKeyboardExtensionDefaultPredicate(&consumePreference);
}

void preferForNextEntry() { gPreferNextEntry = true; }

}  // namespace midad_arabic_keyboard
