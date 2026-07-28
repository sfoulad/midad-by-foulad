#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/MidadLogo120.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Brand lockup: mark, then "MIDAD" over "By Foulad". Both are the wordmark
  // rather than prose -- they read the same in every language, exactly as
  // STR_CROSSPOINT already did -- so they go through tr() to keep every
  // user-facing string in one place, not because either gets translated.
  renderer.clearScreen();
  renderer.drawImage(MidadLogo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_BRAND_MIDAD), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 92, tr(STR_BRAND_BY_FOULAD));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 114, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
