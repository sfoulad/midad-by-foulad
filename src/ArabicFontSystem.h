#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

class GfxRenderer;

/// Supplies Arabic glyphs for GfxRenderer::drawArabicText/getArabicTextWidth. Two
/// independent axes: a built-in family+size pair (CrossPointSettings::arabicFontFamily/
/// arabicFontSize, one of 5 bundled OFL fonts at 12/14/16/18pt) used for EPUB reading
/// text, plus a fixed set of small UI-context sizes (8/10/12pt, always Noto Sans
/// Arabic) used for titles/menus/lists. An optional SD-card override
/// (sdArabicFontFamilyName) replaces BOTH axes uniformly with one custom font loaded
/// at one fixed size, mirroring SdCardFontSystem's discover/load pattern for the
/// reading font.
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

  /// Resolves one of the 5 built-in Arabic reading-font families (see
  /// CrossPointSettings::ARABIC_FONT_FAMILY) at one of the 4 reading sizes (see
  /// CrossPointSettings::FONT_SIZE) to its font ID. Exposed so the Settings UI can
  /// preview a font choice before committing it.
  static int resolveBuiltinReadingFontId(uint8_t family, uint8_t size);

  /// Mark the registry as needing re-discovery (e.g. after a web-server font upload).
  void markRegistryDirty() { registryDirty_ = true; }

 private:
  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  bool registryDirty_ = false;
};

// Global Arabic font system instance (defined in main.cpp).
extern ArabicFontSystem arabicFontSystem;
