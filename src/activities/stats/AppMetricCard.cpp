#include "AppMetricCard.h"

#include <I18n.h>

#include "fontIds.h"

namespace {
void drawCheckBadge(const GfxRenderer& renderer, const int x, const int y) {
  renderer.fillRect(x, y, 18, 18, true);
  renderer.drawLine(x + 4, y + 10, x + 7, y + 13, 2, false);
  renderer.drawLine(x + 7, y + 13, x + 13, y + 5, 2, false);
}
}  // namespace

namespace AppMetricCard {

void draw(const GfxRenderer& renderer, const Rect& rect, const char* label, const char* value, const Options& options) {
  renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  const bool rtl = I18N.isRtl();
  const int textWidth = rect.width - options.contentInset;

  // Shrink the value a size when it would overflow the card.
  const int valueFontId =
      renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD) > textWidth ? UI_10_FONT_ID : UI_12_FONT_ID;
  const std::string shownValue = renderer.truncatedText(valueFontId, value, textWidth, EpdFontFamily::BOLD);
  const int valueX = rtl ? rect.x + rect.width - options.paddingX -
                               renderer.getTextWidth(valueFontId, shownValue.c_str(), EpdFontFamily::BOLD)
                         : rect.x + options.paddingX;
  renderer.drawText(valueFontId, valueX, rect.y + options.valueY + (valueFontId == UI_12_FONT_ID ? 0 : 3),
                    shownValue.c_str(), true, EpdFontFamily::BOLD);

  const std::string shownLabel = renderer.truncatedText(UI_10_FONT_ID, label, textWidth);
  const int labelX =
      rtl ? rect.x + rect.width - options.paddingX - renderer.getTextWidth(UI_10_FONT_ID, shownLabel.c_str())
          : rect.x + options.paddingX;
  renderer.drawText(UI_10_FONT_ID, labelX, rect.y + options.labelY, shownLabel.c_str());

  if (options.showCheck) {
    const int badgeX = rtl ? rect.x + 10 : rect.x + rect.width - 28;
    drawCheckBadge(renderer, badgeX, rect.y + rect.height - 28);
  }
}

}  // namespace AppMetricCard
