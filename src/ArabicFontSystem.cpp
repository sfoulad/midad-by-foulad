#include "ArabicFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {
// Arabic text renders at a fixed size regardless of the surrounding UI font's own
// size (titles/chapter lists use a handful of fixed UI font IDs, not the reader's
// variable font size) -- MEDIUM is a reasonable general-purpose match.
constexpr uint8_t ARABIC_FONT_SIZE_ENUM = CrossPointSettings::MEDIUM;
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
    renderer.setArabicFontId(NOTOSANSARABIC_14_FONT_ID);
    return;
  }

  if (currentFamily == wantedFamily) {
    renderer.setArabicFontId(manager_.getFontId(wantedFamily));
    return;
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family && manager_.loadFamily(*family, renderer, ARABIC_FONT_SIZE_ENUM)) {
    LOG_DBG("ARFS", "Loaded Arabic font family: %s", wantedFamily);
    renderer.setArabicFontId(manager_.getFontId(wantedFamily));
  } else {
    LOG_ERR("ARFS", "Failed to load Arabic font family: %s (falling back to built-in)", wantedFamily);
    SETTINGS.sdArabicFontFamilyName[0] = '\0';
    renderer.setArabicFontId(NOTOSANSARABIC_14_FONT_ID);
  }
}
