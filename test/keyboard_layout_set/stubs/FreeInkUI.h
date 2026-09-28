#pragma once

#include <cstdint>

// Only the layout enum matters here; the rest of FreeInkUI is display code that
// KeyboardLayoutSet never touches. Members mirror the SDK's order because the
// table's position-to-bit mapping is what these tests are about.
namespace freeink {
namespace ui {

enum class KeyboardLayoutId : uint8_t {
  QwertyEn,
  AzertyFr,
  QwertzDe,
  SpanishEs,
  CyrillicRu,
  CyrillicUk,
  CyrillicBe,
  CyrillicKk,
  HebrewIl,
  ArabicAr,
};

}  // namespace ui
}  // namespace freeink
