#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

class GfxRenderer;

/// Supplies Arabic glyphs for GfxRenderer::drawArabicText/getArabicTextWidth. Two
/// independent axes: a READING font (built-in Noto Naskh Arabic at
/// CrossPointSettings::arabicFontSize, or the optional SD-card override
/// sdArabicFontFamilyName loaded at that same size) used for EPUB body text and any
/// other fontId without an explicit mapping, plus a fixed set of small UI-context
/// sizes (8/10/12pt, always built-in Noto Sans Arabic) used for titles/menus/lists.
/// The UI axis is never overridden by the SD font -- UI geometry is sized for the
/// small built-in tiers, and this keeps Arabic Font Size purely a reading-text knob.
class ArabicFontSystem {
 public:
  ArabicFontSystem() = default;
  ArabicFontSystem(const ArabicFontSystem&) = delete;
  ArabicFontSystem& operator=(const ArabicFontSystem&) = delete;

  /// Discover SD card fonts and load the user's saved Arabic font selection, if any.
  /// Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Apply only the built-in Arabic UI mappings (the 8/10/12pt tiers plus a built-in
  /// reading default), skipping the SD-card registry scan and any SD family load.
  /// For boot paths that only ever paint firmware UI and then reboot -- the OTA
  /// screens -- where the reading font can never be used but Arabic UI text still
  /// has to render. Not a substitute for begin() on any path that reaches the reader.
  void beginUiOnly(GfxRenderer& renderer);

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
  // Size enum the current SD family was loaded at, so an Arabic Font Size change
  // reloads the family instead of matching on name alone and keeping the old size.
  uint8_t loadedSdSizeEnum_ = 0xFF;
};

// Global Arabic font system instance (defined in main.cpp).
extern ArabicFontSystem arabicFontSystem;
