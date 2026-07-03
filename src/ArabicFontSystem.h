#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

class GfxRenderer;

/// Loads a single, user-chosen SD-card font family to supply Arabic glyphs for
/// GfxRenderer::drawArabicText/getArabicTextWidth (via GfxRenderer::setArabicFontId),
/// independent of whatever font is selected for reading (SdCardFontSystem). Mirrors
/// SdCardFontSystem's discover/load pattern, but only ever loads one fixed size --
/// Arabic text always renders at a fixed UI-ish size, not the reader's variable font
/// size, so there's no per-size matching to do.
class ArabicFontSystem {
 public:
  ArabicFontSystem() = default;
  ArabicFontSystem(const ArabicFontSystem&) = delete;
  ArabicFontSystem& operator=(const ArabicFontSystem&) = delete;

  /// Discover SD card fonts and load the user's saved Arabic font selection, if any.
  /// Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Re-apply after CrossPointSettings::sdArabicFontFamilyName changes (e.g. from the
  /// Settings UI) or the SD font registry changes.
  void ensureLoaded(GfxRenderer& renderer);

  /// Access the registry (e.g. for the Settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Mark the registry as needing re-discovery (e.g. after a web-server font upload).
  void markRegistryDirty() { registryDirty_ = true; }

 private:
  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  bool registryDirty_ = false;
};

// Global Arabic font system instance (defined in main.cpp).
extern ArabicFontSystem arabicFontSystem;
