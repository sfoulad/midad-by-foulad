#pragma once

#include <FreeInkUI.h>
#include <I18n.h>

#include <cstdint>

namespace keyboard_layouts {

struct LayoutInfo {
  freeink::ui::KeyboardLayoutId id;
  Language language;
  // Carries Latin letters. Credentials, URLs and WiFi passwords are Latin, and
  // no symbol layer has letters, so at least one such layout must stay enabled.
  bool latin;
};

// Table position is the persisted bit assignment. Keep existing rows in place
// and append new layouts so SDK enum changes cannot reinterpret saved masks.
inline constexpr LayoutInfo ALL[] = {
    // Only layouts whose Language this fork ships. Midad carries EN/AR/BG/FA
    // (the rest were dropped to cut firmware size), and the SDK has no Arabic,
    // Bulgarian or Persian keyboard, so those fall back to English.
    //
    // Nothing opens a FreeInkUI keyboard in this fork yet, so this mask only
    // drives the settings screen -- startingLayout() and next() have no callers.
    // Arabic input runs through KeyboardEntryActivity's own panel on every
    // board. That panel retires only once the FUI keyboard is wired up here.
    {freeink::ui::KeyboardLayoutId::QwertyEn, Language::EN, true},
};
inline constexpr uint8_t COUNT = sizeof(ALL) / sizeof(ALL[0]);
static_assert(COUNT <= 16, "keyboard layout mask is uint16_t");

inline constexpr uint16_t bitAt(const uint8_t i) { return static_cast<uint16_t>(1u << i); }

// Derived from the table rather than written out, so appending a non-Latin
// layout cannot leave it claimed as Latin. A hand-written span did exactly
// that: it covered bits 0-3 as headroom, so the first non-Latin row appended
// after English satisfied the "keep one Latin layout" guard by itself.
inline constexpr uint16_t latinBits() {
  uint16_t bits = 0;
  for (uint8_t i = 0; i < COUNT; ++i) {
    if (ALL[i].latin) bits = static_cast<uint16_t>(bits | bitAt(i));
  }
  return bits;
}
inline constexpr uint16_t LATIN_BITS = latinBits();
static_assert(LATIN_BITS != 0, "at least one Latin layout must ship: symbol layers have no letters");

uint16_t enabled();
freeink::ui::KeyboardLayoutId startingLayout();
freeink::ui::KeyboardLayoutId next(freeink::ui::KeyboardLayoutId current);

}  // namespace keyboard_layouts
