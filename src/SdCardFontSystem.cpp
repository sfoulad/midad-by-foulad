#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"

namespace {

// Snap the point size actually in effect to `availablePointSize` (the nearest
// size the active family can render) and persist the snap, so the settings UI
// (and next boot) show the real value rather than one nothing renders at.
//
// The correction target mirrors effFontSize()'s own source of truth: when a
// book-level SIZE override is active, the wanted size came from
// SETTINGS.bookFontSize (RAM-only, see CrossPointSettings' per-book block), so
// the correction lands there too. A book-level FAMILY override alone does not
// redirect the correction -- the wanted size in that case still came from the
// global fontPointSize, and persisting a synthetic per-book size override would
// silently freeze this book's rendered size against future global changes. No-op
// when availablePointSize is 0 (lookup found nothing) or already matches.
void snapEffectiveFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0) return;
  const bool bookSizeOverrideActive = SETTINGS.bookFontSize != CrossPointSettings::BOOK_NO_OVERRIDE;
  if (bookSizeOverrideActive) {
    if (SETTINGS.bookFontSize != availablePointSize) {
      LOG_DBG("SDFS", "Per-book font size unavailable, snapping to %u", availablePointSize);
      SETTINGS.bookFontSize = availablePointSize;
    }
    return;
  }
  if (availablePointSize == SETTINGS.fontPointSize) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}

// A family failed to load / disappeared: clear whichever setting asked for it.
// A bad per-book override must not wipe the user's global font choice.
void clearWantedFamily() {
  if (SETTINGS.bookSdFontFamilyName[0] != '\0') {
    SETTINGS.bookSdFontFamilyName[0] = '\0';
  } else {
    SETTINGS.clearSdFontFamily();
  }
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it. eff* so a per-book override
  // set before begin() (should not normally happen at boot, but keeps this
  // consistent with ensureLoaded()) still wins.
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, SETTINGS.effFontSize())) {
        snapEffectiveFontPointSizeTo(manager_.currentPointSize());
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.clearSdFontFamily();
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  // eff* so per-book overrides win (see CrossPointSettings per-book block).
  const char* wantedFamily = SETTINGS.effSdFontFamilyName();
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t wantedPointSize = SETTINGS.effFontSize();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    // No SD family active (globally or per-book): make sure the point size in
    // effect is one the built-in family actually ships.
    snapEffectiveFontPointSizeTo(
        snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), wantedPointSize));
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      clearWantedFamily();
      return;
    }
    const auto* selected = family->findNearestSize(wantedPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    snapEffectiveFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, wantedPointSize)) {
      snapEffectiveFontPointSizeTo(manager_.currentPointSize());
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      clearWantedFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    clearWantedFamily();
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontPointSize), so
  // the requested size is implicit — always return the single loaded font ID
  // for this family. ensureLoaded() must have been called with the current
  // settings before this.
  return manager_.getFontId(familyName);
}
