#pragma once

#include <cstddef>
#include <cstdint>

// Logical-rect -> physical-framebuffer geometry, split out of GfxRenderer so the
// snapshot sizing it feeds (HomeActivity's cover-tile buffer) can be budgeted and
// unit-tested against a real panel geometry instead of a re-derived formula.
// Header-only and free of Arduino/HAL dependencies for exactly that reason.
namespace RegionGeometry {

// Values match GfxRenderer::Orientation one for one (static_assert'd in
// GfxRenderer.cpp); kept as a separate enum so this header stays dependency-free.
enum Orientation {
  Portrait = 0,
  LandscapeClockwise = 1,
  PortraitInverted = 2,
  LandscapeCounterClockwise = 3,
};

inline void rotate(const int orientation, const int x, const int y, int* phyX, int* phyY, const uint16_t panelWidth,
                   const uint16_t panelHeight) {
  switch (orientation) {
    case Portrait:
      // Logical portrait -> panel, rotated 90 degrees clockwise
      *phyX = y;
      *phyY = panelHeight - 1 - x;
      break;
    case LandscapeClockwise:
      // Logical landscape rotated 180 degrees (swap top/bottom and left/right)
      *phyX = panelWidth - 1 - x;
      *phyY = panelHeight - 1 - y;
      break;
    case PortraitInverted:
      // Logical portrait -> panel, rotated 90 degrees counter-clockwise
      *phyX = panelWidth - 1 - y;
      *phyY = x;
      break;
    case LandscapeCounterClockwise:
    default:
      // Logical landscape aligned with panel orientation
      *phyX = x;
      *phyY = y;
      break;
  }
}

// Bounding box of the logical rect's four rotated corners on the physical panel.
// Output coords are inclusive and clamped; false if the rect ends up fully off-panel.
inline bool physicalBounds(const int orientation, const int lx, const int ly, const int lw, const int lh,
                           const uint16_t panelWidth, const uint16_t panelHeight, int* outX0, int* outY0, int* outX1,
                           int* outY1) {
  if (lw <= 0 || lh <= 0) return false;
  int minX = INT32_MAX;
  int minY = INT32_MAX;
  int maxX = INT32_MIN;
  int maxY = INT32_MIN;
  const int corners[4][2] = {{lx, ly}, {lx + lw - 1, ly}, {lx, ly + lh - 1}, {lx + lw - 1, ly + lh - 1}};
  for (auto& c : corners) {
    int phyX;
    int phyY;
    rotate(orientation, c[0], c[1], &phyX, &phyY, panelWidth, panelHeight);
    if (phyX < minX) minX = phyX;
    if (phyY < minY) minY = phyY;
    if (phyX > maxX) maxX = phyX;
    if (phyY > maxY) maxY = phyY;
  }
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  if (maxX >= panelWidth) maxX = panelWidth - 1;
  if (maxY >= panelHeight) maxY = panelHeight - 1;
  if (minX > maxX || minY > maxY) return false;
  *outX0 = minX;
  *outY0 = minY;
  *outX1 = maxX;
  *outY1 = maxY;
  return true;
}

// Bytes needed to hold a copy of the rect's 1-bit framebuffer rows. x bounds are
// widened to byte boundaries on either side so per-row memcpy stays byte-aligned
// even when the logical rect doesn't land on one.
inline size_t byteSize(const int orientation, const int lx, const int ly, const int lw, const int lh,
                       const uint16_t panelWidth, const uint16_t panelHeight) {
  int x0, y0, x1, y1;
  if (!physicalBounds(orientation, lx, ly, lw, lh, panelWidth, panelHeight, &x0, &y0, &x1, &y1)) {
    return 0;
  }
  const int bytesPerRow = x1 / 8 - x0 / 8 + 1;
  const int rowCount = y1 - y0 + 1;
  return static_cast<size_t>(bytesPerRow) * static_cast<size_t>(rowCount);
}

}  // namespace RegionGeometry
