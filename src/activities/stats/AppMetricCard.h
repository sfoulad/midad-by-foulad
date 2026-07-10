#pragma once
#include <GfxRenderer.h>

#include "components/themes/BaseTheme.h"  // Rect

// Dithered metric card (bold value over label), ported from cpr-vcodex's
// AppMetricCard with RTL mirroring for the Arabic UI. Value/label are plain
// C strings (no std::string in the draw path).
namespace AppMetricCard {

struct Options {
  int paddingX = 12;
  int contentInset = 24;
  int valueY = 12;
  int labelY = 44;
  bool showCheck = false;  // goal-met badge in the value corner
};

void draw(const GfxRenderer& renderer, const Rect& rect, const char* label, const char* value,
          const Options& options = {});

}  // namespace AppMetricCard
