#pragma once

// Midad's Arabic keyboard layer, supplied to CrossPoint's keyboard through the
// generic extension point (activities/util/KeyboardExtension.h, CrossPoint PR
// #3280). Nothing about Arabic lives in CrossPoint: it stores three function
// pointers and draws whatever this file gives it.
//
// Registered once at startup from main.cpp. Before that call CrossPoint's
// keyboard behaves exactly as it does upstream, which is what keeps the X3/X4
// button keyboard unchanged for anyone not using the Arabic layer.
namespace midad_arabic_keyboard {

// Installs the three hooks. Call once during setup().
void install();

// One-shot hint that the NEXT keyboard opened should start on the Arabic layer
// rather than requiring two presses of the mode key. Replaces the `preferArabic`
// constructor argument Midad's previous keyboard carried: the upstream
// constructor has no such parameter and the extension's default predicate takes
// no arguments, so the caller sets this immediately before opening the keyboard
// and the predicate consumes it. Same semantics as before -- it describes the
// field, not the user, and is not persisted.
void preferForNextEntry();

}  // namespace midad_arabic_keyboard
