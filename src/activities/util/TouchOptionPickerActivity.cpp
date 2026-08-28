#include "TouchOptionPickerActivity.h"

#if FREEINK_CAP_TOUCH

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

TouchOptionPickerActivity::TouchOptionPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::string title, std::vector<std::string> options,
                                                     const int currentIndex)
    : UiListActivity("TouchOptionPicker", renderer, mappedInput),
      title(std::move(title)),
      options(std::move(options)),
      currentIndex(currentIndex) {}

void TouchOptionPickerActivity::onEnter() {
  UiListActivity::onEnter();
  RenderLock lock(*this);
  rowItems.clear();
  rowItems.reserve(options.size());
  for (const auto& option : options) {
    fui::ListItem item;
    item.label = option.c_str();
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
  nav.reset(currentIndex >= 0 && currentIndex < static_cast<int>(options.size()) ? currentIndex : 0);
}

void TouchOptionPickerActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  TouchOptionPickerResult result;
  result.selectedIndex = index;
  setResult(std::move(result));
  finish();
}

void TouchOptionPickerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.selectionMarker = fui::SelectionMarker::Underline;
  syncListViewport(screen, props, /*hasSubtitle=*/false);
  screen.list(props);
}

#endif  // FREEINK_CAP_TOUCH
