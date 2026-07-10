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

// [family][size] -> font ID, for the built-in Arabic reading-font family at the
// 4 reading sizes (12/14/16/18pt = Small/Medium/Large/X-Large). Order matches
// CrossPointSettings::ARABIC_FONT_FAMILY and FONT_SIZE. Only Noto Naskh Arabic
// ships in flash (other families were trimmed to keep OTA images small).
constexpr int
    kBuiltinArabicReadingFontIds[CrossPointSettings::ARABIC_FONT_FAMILY_COUNT][CrossPointSettings::FONT_SIZE_COUNT] = {
        {NOTONASKHARABIC_12_FONT_ID, NOTONASKHARABIC_14_FONT_ID, NOTONASKHARABIC_16_FONT_ID,
         NOTONASKHARABIC_18_FONT_ID},
};

// Registers the built-in Arabic font at each size actually used for Arabic-eligible
// UI text -- SMALL_FONT_ID (8pt), UI_10_FONT_ID (10pt), UI_12_FONT_ID (12pt) -- so
// Arabic renders at the same size/baseline as the surrounding Latin text instead of
// one fixed size that overflows small rows or grid cells. Also maps every Latin
// reading font ID (Noto Serif/Sans x 12/14/16/18) to the user's chosen Arabic reading
// family+size, independent of which Latin reading font is selected -- Arabic body
// text tracks its own family/size preference, not the Latin one. Falls back to the
// 12pt tier as the catch-all for any other fontId.
void registerBuiltinSizeMappings(GfxRenderer& renderer) {
  // matchLatinBaseline=true: UI labels sit on the requested Latin font's baseline so
  // Arabic strings fit the fixed Latin-sized UI geometry (30px list rows, button
  // hints, header) and align with adjacent Latin text -- essential for a fully-Arabic
  // interface. Reading mappings below stay unmatched: EPUB rows are sized for the
  // full Arabic line height and book text carries diacritics needing that headroom.
  renderer.setArabicFontIdForFontId(SMALL_FONT_ID, NOTOSANSARABIC_8_FONT_ID, /*matchLatinBaseline=*/true);
  renderer.setArabicFontIdForFontId(UI_10_FONT_ID, NOTOSANSARABIC_10_FONT_ID, /*matchLatinBaseline=*/true);
  renderer.setArabicFontIdForFontId(UI_12_FONT_ID, NOTOSANSARABIC_12_FONT_ID, /*matchLatinBaseline=*/true);

  const int readingFontId =
      ArabicFontSystem::resolveBuiltinReadingFontId(SETTINGS.arabicFontFamily, SETTINGS.arabicFontSize);
  for (const int latinReadingFontId :
       {NOTOSERIF_12_FONT_ID, NOTOSERIF_14_FONT_ID, NOTOSERIF_16_FONT_ID, NOTOSERIF_18_FONT_ID}) {
    renderer.setArabicFontIdForFontId(latinReadingFontId, readingFontId);
  }

  renderer.setArabicFontId(NOTOSANSARABIC_12_FONT_ID);
}
}  // namespace

int ArabicFontSystem::resolveBuiltinReadingFontId(uint8_t family, uint8_t size) {
  if (family >= CrossPointSettings::ARABIC_FONT_FAMILY_COUNT) family = CrossPointSettings::NOTONASKHARABIC;
  if (size >= CrossPointSettings::FONT_SIZE_COUNT) size = CrossPointSettings::MEDIUM;
  return kBuiltinArabicReadingFontIds[family][size];
}

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
