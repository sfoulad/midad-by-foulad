#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>

#include "fontIds.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  // Wrapped, not one line: this activity takes an arbitrary caller-supplied string, and
  // drawCenteredText neither wraps nor truncates. Anything wider than the panel overruns
  // it, and because the draw is centered the overrun clips at *both* ends -- the middle of
  // the sentence survives with its start and finish gone. The only current caller passes a
  // short hardcoded "SD card error" that fits, so this is latent rather than visible
  // today; but the point of the class is that callers supply their own text, and it breaks
  // silently the moment that string is translated or lengthened.
  const int maxWidth = renderer.getScreenWidth() - margin * 2;
  // Measured before drawing because the block has to be centered as a block, and its
  // height is not known until the wrap is computed. Centering on a single line's height
  // and then drawing two pushes the text off-center by half a line.
  const int height = renderer.measureWrappedTextHeight(fontId, maxWidth, text.c_str(), maxLines, style);
  const int top = (renderer.getScreenHeight() - height) / 2;

  renderer.clearScreen();
  renderer.drawCenteredTextWrapped(fontId, top, maxWidth, text.c_str(), maxLines, true, style);
  renderer.displayBuffer(refreshMode);
}
