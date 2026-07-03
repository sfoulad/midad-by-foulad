#include "ArabicFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {
// SD-card Arabic font overrides load a single size for the whole family --
// SdCardFontManager's smallest available tier (SMALL=12pt) is the closest match to
// the built-in default's UI_12_FONT_ID tier, the largest of the three sizes Arabic
// UI text actually needs (see registerBuiltinSizeMappings() below).
constexpr uint8_t ARABIC_FONT_SIZE_ENUM = CrossPointSettings::SMALL;

// Registers the built-in Arabic font at each size actually used for Arabic-eligible
// UI text -- SMALL_FONT_ID (8pt), UI_10_FONT_ID (10pt), UI_12_FONT_ID (12pt) -- so
// Arabic renders at the same size/baseline as the surrounding Latin text instead of
// one fixed size that overflows small rows or grid cells. Also sets arabicFontId_ (the
// catch-all fallback for any other fontId) to the 12pt tier.
void registerBuiltinSizeMappings(GfxRenderer& renderer) {
  renderer.setArabicFontIdForFontId(SMALL_FONT_ID, NOTOSANSARABIC_8_FONT_ID);
  renderer.setArabicFontIdForFontId(UI_10_FONT_ID, NOTOSANSARABIC_10_FONT_ID);
  renderer.setArabicFontIdForFontId(UI_12_FONT_ID, NOTOSANSARABIC_12_FONT_ID);
  renderer.setArabicFontId(NOTOSANSARABIC_12_FONT_ID);
}
}  // namespace

void ArabicFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();
  ensureLoaded(renderer);
  LOG_DBG("ARFS", "Arabic font system ready (%d families discovered)", registry_.getFamilyCount());
}

void ArabicFontSystem::ensureLoaded(GfxRenderer& renderer) {
  if (registryDirty_) {
    registryDirty_ = false;
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdArabicFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    // No SD override selected -- fall back to the built-in Noto Sans Arabic font
    // bundled in flash (see main.cpp), so Arabic renders out of the box with zero
    // setup. This differs from the reading-font system: there is always a usable
    // Arabic font, never an empty/off state.
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    registerBuiltinSizeMappings(renderer);
    return;
  }

  if (currentFamily == wantedFamily) {
    // Custom SD font applies uniformly at its one loaded size -- clear the
    // per-fontId mappings so every caller (regardless of requested fontId) falls
    // through to this single arabicFontId_ instead of the built-in size tiers.
    renderer.clearArabicFontIdMappings();
    renderer.setArabicFontId(manager_.getFontId(wantedFamily));
    return;
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family && manager_.loadFamily(*family, renderer, ARABIC_FONT_SIZE_ENUM)) {
    LOG_DBG("ARFS", "Loaded Arabic font family: %s", wantedFamily);
    renderer.clearArabicFontIdMappings();
    renderer.setArabicFontId(manager_.getFontId(wantedFamily));
  } else {
    LOG_ERR("ARFS", "Failed to load Arabic font family: %s (falling back to built-in)", wantedFamily);
    SETTINGS.sdArabicFontFamilyName[0] = '\0';
    registerBuiltinSizeMappings(renderer);
  }
}
