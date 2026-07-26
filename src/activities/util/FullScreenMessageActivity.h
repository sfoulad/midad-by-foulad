#pragma once
#include <EpdFontFamily.h>
#include <HalDisplay.h>

#include <string>
#include <utility>

#include "activities/Activity.h"
#include "fontIds.h"

class FullScreenMessageActivity final : public Activity {
  std::string text;
  EpdFontFamily::Style style;
  HalDisplay::RefreshMode refreshMode;
  // Same side margin ConfirmationActivity uses, so the two message screens agree on
  // where text stops.
  static constexpr int margin = 20;
  static constexpr int fontId = UI_10_FONT_ID;
  // Generous: this is a whole blank screen, so there is room, and the cap exists only to
  // stop a pathological string from running past the bottom edge. Overflow past it is
  // ellipsized rather than dropped (see drawCenteredTextWrapped).
  static constexpr int maxLines = 6;

 public:
  explicit FullScreenMessageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text,
                                     const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                     const HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH)
      : Activity("FullScreenMessage", renderer, mappedInput),
        text(std::move(text)),
        style(style),
        refreshMode(refreshMode) {}
  void onEnter() override;
};
