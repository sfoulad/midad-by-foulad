#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

/// Picks the font family used for Arabic text everywhere it appears (book titles,
/// author names, filenames, chapter titles, and EPUB reading text) -- independent of
/// the Latin reading font. Mirrors FontSelectionActivity: 5 built-in Arabic fonts
/// (Settings -> Reader -> Arabic Font Size controls their reading size) plus any
/// SD-card Arabic fonts the user has installed, with a live preview pane.
class ArabicFontSelectionActivity final : public Activity {
 public:
  explicit ArabicFontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const SdCardFontRegistry* registry);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();
  void renderPreviewPane(int top, int height, int fontId, const char* fontName) const;

  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;  // ARABIC_FONT_FAMILY value (builtin) or kSelectableArabicFontCount + sdIdx
  };

  const SdCardFontRegistry* registry_;
  ButtonNavigator buttonNavigator_;
  std::vector<FontEntry> fonts_;
  int selectedIndex_ = 0;
  int previewFontIndex_ = 0;
  uint8_t originalArabicFontFamily_ = 0;
  char originalSdArabicFontFamilyName_[32] = {};

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
};
