#include "ArabicFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {
// [family][size] -> font ID, for the built-in Arabic reading-font family at the
// 4 reading sizes (12/14/16/18pt = Small/Medium/Large/X-Large). Order matches
// CrossPointSettings::ARABIC_FONT_FAMILY and FONT_SIZE.
constexpr int
    kBuiltinArabicReadingFontIds[CrossPointSettings::ARABIC_FONT_FAMILY_COUNT][CrossPointSettings::FONT_SIZE_COUNT] = {
        {NOTONASKHARABIC_12_FONT_ID, NOTONASKHARABIC_14_FONT_ID, NOTONASKHARABIC_16_FONT_ID,
         NOTONASKHARABIC_18_FONT_ID},
        // Amiri's own glyph data was dropped long ago; the row stays and aliases Naskh
        // so a persisted AMIRI=1 keeps meaning a real Arabic book face.
        {NOTONASKHARABIC_12_FONT_ID, NOTONASKHARABIC_14_FONT_ID, NOTONASKHARABIC_16_FONT_ID,
         NOTONASKHARABIC_18_FONT_ID},
        // KFGQPC Uthmanic Hafs: the Madinah Mushaf's own typeface -- the Quran's
        // default reading face (per-book sidecar written at extraction; see
        // QuranBook::ensureExtracted).
        {UTHMANICHAFS_12_FONT_ID, UTHMANICHAFS_14_FONT_ID, UTHMANICHAFS_16_FONT_ID, UTHMANICHAFS_18_FONT_ID},
        // Tajawal (Boutros International, OFL-licensed): a modern geometric-sans
        // reading option, alongside the two traditional book-printing styles above.
        {TAJAWAL_12_FONT_ID, TAJAWAL_14_FONT_ID, TAJAWAL_16_FONT_ID, TAJAWAL_18_FONT_ID},
};

// One mapping scheme for both the built-in and SD-override cases: the three
// Arabic-eligible UI tiers -- SMALL_FONT_ID (8pt), UI_10_FONT_ID (10pt),
// UI_12_FONT_ID (12pt) -- always render in built-in Tajawal at the matching
// size, and EVERYTHING ELSE falls through to readingFontId as the renderer's
// default. Tajawal (not Noto Sans Arabic) so the whole interface -- headers,
// button hints, list rows, grid titles -- reads in one consistent typeface,
// per explicit user request to use Tajawal for all firmware Arabic UI, not
// just reading text. The previous scheme enumerated the four built-in Noto
// Serif reading ids explicitly with Sans-12 as the catch-all, which silently
// pinned Arabic body text to 12pt UI type for anyone reading with an SD-card
// Latin font -- Arabic Font Size appeared to do nothing (user report, and the
// reason SD Arabic overrides ignoring the size setting went unnoticed too).
// SdCardFontManager::loadFamily() now takes a physical point size (the LATIN
// reader's point-size migration -- see ReaderFontSizes.h), not a FONT_SIZE enum
// slot. Arabic sizing is UNTOUCHED by that migration -- arabicFontSize /
// effArabicFontSize() still return a SMALL/MEDIUM/LARGE/EXTRA_LARGE slot -- so
// translate to the slot's target point size here rather than changing what
// effArabicFontSize() returns. Matches kBuiltinArabicReadingFontIds' own column
// order (12/14/16/18 = Small/Medium/Large/X-Large).
uint8_t arabicSizeEnumToPointSize(const uint8_t sizeEnum) {
  switch (sizeEnum) {
    case CrossPointSettings::SMALL:
      return 12;
    case CrossPointSettings::LARGE:
      return 16;
    case CrossPointSettings::EXTRA_LARGE:
      return 18;
    case CrossPointSettings::MEDIUM:
    default:
      return 14;
  }
}

void applyArabicMappings(GfxRenderer& renderer, const int readingFontId) {
  renderer.clearArabicFontIdMappings();
  // matchLatinBaseline=true: UI labels sit on the requested Latin font's baseline so
  // Arabic strings fit the fixed Latin-sized UI geometry (30px list rows, button
  // hints, header) and align with adjacent Latin text -- essential for a fully-Arabic
  // interface. The reading default stays unmatched: EPUB rows are sized for the
  // full Arabic line height and book text carries diacritics needing that headroom.
  renderer.setArabicFontIdForFontId(SMALL_FONT_ID, TAJAWAL_8_FONT_ID, /*matchLatinBaseline=*/true);
  renderer.setArabicFontIdForFontId(UI_10_FONT_ID, TAJAWAL_10_FONT_ID, /*matchLatinBaseline=*/true);
  renderer.setArabicFontIdForFontId(UI_12_FONT_ID, TAJAWAL_12_FONT_ID, /*matchLatinBaseline=*/true);
  renderer.setArabicFontId(readingFontId);
  // Ayah-marker digits always come from this built-in font, never readingFontId --
  // see setArabicDigitFallbackFontId's comment for why (some reading fonts simply
  // don't have Arabic-Indic digit glyphs).
  renderer.setArabicDigitFallbackFontId(NOTOSANSARABIC_12_FONT_ID);
  // Bismillah always comes from the dedicated QuranCommon font -- see
  // setBismillahFontId's comment for why (UthmanicHafs itself has no glyph for the
  // real ligature, U+FDFD). Single fixed 18pt size regardless of Arabic Font Size:
  // the glyph is a whole vocalized phrase baked into one wide glyph, close to the
  // uint8_t width-field cap already at 18pt, so it can't scale per reading tier --
  // see the QuranCommon conversion comment in convert-builtin-fonts.sh.
  renderer.setBismillahFontId(QURANCOMMON_18_FONT_ID);
  // Surah banner always comes from the dedicated SurahBannerV4 font -- see
  // setSurahBannerFontId's comment for why (surah-name-v4.ttf has no cmap entries
  // at all, so no reading font could ever substitute). Single fixed 24pt size
  // regardless of Arabic Font Size, same reasoning as the Bismillah: this is a
  // once-per-surah chrome element, not line-by-line reading text.
  renderer.setSurahBannerFontId(SURAHBANNER_24_FONT_ID);
  // Surah banner's caption labels (ayah count, revelation order) come from the
  // smallest built-in Arabic font, not the active reading font -- see
  // setSurahBannerLabelFontId's comment for why (rasterized at its own small
  // size, not the reading font scaled down at draw time).
  renderer.setSurahBannerLabelFontId(NOTOSANSARABIC_8_FONT_ID);
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

void ArabicFontSystem::beginUiOnly(GfxRenderer& renderer) {
  // Built-in reading font as the fallthrough default, exactly as ensureLoaded's
  // no-SD-override branch does -- an SD override can't be honoured without the
  // registry scan this path exists to avoid, and nothing on an OTA boot draws
  // reading text anyway. The UI tiers (SMALL/UI_10/UI_12 -> Tajawal) are the part
  // that matters here: without them an Arabic-language interface renders no glyphs.
  applyArabicMappings(renderer,
                      resolveBuiltinReadingFontId(SETTINGS.effArabicFontFamily(), SETTINGS.effArabicFontSize()));
  LOG_DBG("ARFS", "Arabic UI mappings applied (SD registry scan skipped)");
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
                        resolveBuiltinReadingFontId(SETTINGS.effArabicFontFamily(), SETTINGS.effArabicFontSize()));
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
  if (family && manager_.loadFamily(*family, renderer, arabicSizeEnumToPointSize(SETTINGS.effArabicFontSize()))) {
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
                        resolveBuiltinReadingFontId(SETTINGS.effArabicFontFamily(), SETTINGS.effArabicFontSize()));
  }
}
