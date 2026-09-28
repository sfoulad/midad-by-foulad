#pragma once

#include <GfxRenderer.h>

#include "components/UITheme.h"

// Midad-owned layout helper for the Midad-only list screens (Dictionary, Gym).
// Lived on UITheme until upstream removed its copy in the FUI conversion; kept
// here so the upstream header stays byte-identical.
inline int midadListItemsPerPage(const GfxRenderer& renderer, const bool hasHeader, const bool hasTabBar,
                                 const bool hasButtonHints, const bool hasSubtitle, const int extraReservedHeight = 0) {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  return UITheme::getInstance().getTheme().getListPageItems(availableHeight, hasSubtitle);
}
