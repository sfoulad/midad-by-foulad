#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// Per-book reading settings, shown as a bottom drawer over the live book page
// (the page stays visible in the top ~20% -- no clearScreen). Every row edits
// the RAM-only SETTINGS.book* overrides (see CrossPointSettings.h); the reader
// persists them to the book's sidecar and re-lays-out when this drawer closes
// with changes. "Global" always means "follow Settings -> Reader".
class BookSettingsActivity final : public Activity {
 public:
  explicit BookSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Row : uint8_t {
    ENGLISH_FONT,
    ENGLISH_FONT_SIZE,
    ARABIC_FONT,
    ARABIC_FONT_SIZE,
    LINE_SPACING,
    ALIGNMENT,
    RESET,
  };

  struct RowInfo {
    Row row;
    StrId labelId;
  };

  void openEditorForRow(Row row);
  std::string valueLabel(Row row) const;
  std::string globalLabel(const char* effectiveValueLabel) const;

  std::vector<RowInfo> rows;
  // SD card family names snapshotted at onEnter (font-family rows only appear
  // when the matching registry has at least one family).
  std::vector<std::string> latinSdFamilies;
  std::vector<std::string> arabicSdFamilies;

  int selectedIndex = 0;
  bool changed = false;

  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
};
