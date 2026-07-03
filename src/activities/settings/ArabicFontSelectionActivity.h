#pragma once

#include <SdCardFontRegistry.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/// Picks the SD-card font family used for Arabic text everywhere it appears (book
/// titles, author names, filenames, chapter titles) -- independent of the reading
/// font. Simpler than FontSelectionActivity: no built-in options (no bundled font
/// carries Arabic glyphs) and no live preview, just a flat list plus "None" to clear
/// the setting.
class ArabicFontSelectionActivity final : public Activity {
 public:
  explicit ArabicFontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const SdCardFontRegistry* registry);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const SdCardFontRegistry* registry_;
  ButtonNavigator buttonNavigator_;
  std::vector<std::string> familyNames_;  // index 0 is always "None"
  int selectedIndex_ = 0;

  void handleSelection();
};
