#pragma once

#include <GfxRenderer.h>

// The "cover" for a grid tile that has no cover image: a solid black cell with the
// label set in the middle, framed and ruled so it reads as a designed cover rather
// than a failure to load one.
//
// Shared because two grids need it for the same reason. My Books uses it for the
// synthetic app tiles (Games, Tasbih, News...), whose paths are never real files;
// the OPDS browser uses it for news feeds, which carry no cover art in their entries.
// Both looked broken with the generic BookIcon placeholder -- an anonymous icon in a
// box reads as an unopened book that failed, not as a thing you are meant to press.
void drawTileCover(GfxRenderer& renderer, int cellX, int cellY, int cellWidth, int cellHeight, const char* label);
