#include "ArabicFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {
// [family][size] -> font ID, for the built-in Arabic reading-font family at the
// 4 reading sizes (12/14/16/18pt = Small/Medium/Large/X-Large). Order matches
// CrossPointSettings::ARABIC_FONT_FAMILY and FONT_SIZE. Only Noto Naskh Arabic
// ships in flash (other families were trimmed to keep OTA images small).
constexpr int
    kBuiltinArabicReadingFontIds[CrossPointSettings::ARABIC_FONT_FAMILY_COUNT][CrossPointSettings::FONT_SIZE_COUNT] = {
        {NOTONASKHARABIC_12_FONT_ID, NOTONASKHARABIC_14_FONT_ID, NOTONASKHARABIC_16_FONT_ID,
         NOTONASKHARABIC_18_FONT_ID},
};

// One mapping scheme for both the built-in and SD-override cases: the three
// Arabic-eligible UI tiers -- SMALL_FONT_ID (8pt), UI_10_FONT_ID (10pt),
// UI_12_FONT_ID (12pt) -- always render in built-in Noto Sans Arabic at the
// matching size, and EVERYTHING ELSE falls through to readingFontId as the
// renderer's default. The previous scheme enumerated the four built-in Noto
// Serif reading ids explicitly with Sans-12 as the catch-all, which silently
// pinned Arabic body text to 12pt UI type for anyone reading with an SD-card
// Latin font -- Arabic Font Size appeared to do nothing (user report, and the
// reason SD Arabic overrides ignoring the size setting went unnoticed too).
void applyArabicMappings(GfxRenderer& renderer, const int readingFontId) {
  renderer.clearArabicFontIdMappings();
  // matchLatinBaseline=true: UI labels sit on the requested Latin font's baseline so
  // Arabic strings fit the fixed Latin-sized UI geometry (30px list rows, button
  // hints, header) and align with adjacent Latin text -- essential for a fully-Arabic
  // interface. The reading default stays unmatched: EPUB rows are sized for the
  // full Arabic line height and book text carries diacritics needing that headroom.
  renderer.setArabicFontIdForFontId(SMALL_FONT_ID, NOTOSANSARABIC_8_FONT_ID, /*matchLatinBaseline=*/true);
  renderer.setArabicFontIdForFontId(UI_10_FONT_ID, NOTOSANSARABIC_10_FONT_ID, /*matchLatinBaseline=*/true);
  renderer.setArabicFontIdForFontId(UI_12_FONT_ID, NOTOSANSARABIC_12_FONT_ID, /*matchLatinBaseline=*/true);
  renderer.setArabicFontId(readingFontId);
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

  // eff* so per-book overrides win (see CrossPointSettings per-book block).
  const char* wantedFamily = SETTINGS.effSdArabicFontFamilyName();
  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    // No SD override selected -- use the built-in Noto Naskh Arabic reading font
    // bundled in flash (see main.cpp) at the user's Arabic Font Size, so Arabic
    // renders out of the box with zero setup. This differs from the reading-font
    // system: there is always a usable Arabic font, never an empty/off state.
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
      loadedSdSizeEnum_ = 0xFF;
    }
    applyArabicMappings(renderer,
                        resolveBuiltinReadingFontId(SETTINGS.arabicFontFamily, SETTINGS.effArabicFontSize()));
    return;
  }

  if (currentFamily == wantedFamily && loadedSdSizeEnum_ == SETTINGS.effArabicFontSize()) {
    // Same family at the same size: just re-apply the mappings. The size check
    // matters -- matching on name alone kept serving the previously loaded size
    // after an Arabic Font Size change (user report: size setting did nothing,
    // even after a cache clear).
    applyArabicMappings(renderer, manager_.getFontId(wantedFamily));
    return;
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
    loadedSdSizeEnum_ = 0xFF;
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family && manager_.loadFamily(*family, renderer, SETTINGS.effArabicFontSize())) {
    LOG_DBG("ARFS", "Loaded Arabic font family: %s (sizeEnum=%u)", wantedFamily, SETTINGS.effArabicFontSize());
    loadedSdSizeEnum_ = SETTINGS.effArabicFontSize();
    applyArabicMappings(renderer, manager_.getFontId(wantedFamily));
  } else {
    LOG_ERR("ARFS", "Failed to load Arabic font family: %s (falling back to built-in)", wantedFamily);
    if (SETTINGS.bookSdArabicFontFamilyName[0] != '\0') {
      SETTINGS.bookSdArabicFontFamilyName[0] = '\0';  // bad per-book override, not the global
    } else {
      SETTINGS.sdArabicFontFamilyName[0] = '\0';
    }
    applyArabicMappings(renderer,
                        resolveBuiltinReadingFontId(SETTINGS.arabicFontFamily, SETTINGS.effArabicFontSize()));
  }
}
