#include "TouchSettingsActivity.h"

#if FREEINK_CAP_TOUCH

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "SettingsGroupMap.h"
#include "TouchSettingsGroupActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/customListIcons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

TouchSettingsActivity::TouchSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("TouchSettings", renderer, mappedInput) {}

void TouchSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  RenderLock lock(*this);
  rebuildRowItems();
}

// One row per TouchSettingsGroup, in enum order. actionValue carries the
// group index straight through to activateIndex()/TouchSettingsGroupActivity
// -- see TouchSettingsGroup in SettingsGroupMap.h.
void TouchSettingsActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(static_cast<size_t>(TouchSettingsGroup::Count));

  auto addRow = [this](const StrId title, const StrId summary, const fui::BitmapRef icon, const TouchSettingsGroup group) {
    fui::ListItem item;
    item.label = I18N.get(title);
    item.subtitle = I18N.get(summary);
    item.icon = icon;
    item.actionValue = static_cast<int16_t>(group);
    rowItems.push_back(item);
  };

  addRow(StrId::STR_TOUCH_GROUP_GENERAL, StrId::STR_TOUCH_GROUP_GENERAL_SUMMARY, listIconFor(UIIcon::Settings, 32),
        TouchSettingsGroup::General);
  addRow(StrId::STR_TOUCH_GROUP_DISPLAY, StrId::STR_TOUCH_GROUP_DISPLAY_SUMMARY, fui::bitmapFromIcon(icon_sun_32),
        TouchSettingsGroup::DisplayLighting);
  addRow(StrId::STR_TOUCH_GROUP_NETWORK, StrId::STR_TOUCH_GROUP_NETWORK_SUMMARY, listIconFor(UIIcon::Wifi, 32),
        TouchSettingsGroup::NetworkBluetooth);
  addRow(StrId::STR_TOUCH_GROUP_READING, StrId::STR_TOUCH_GROUP_READING_SUMMARY, listIconFor(UIIcon::Book, 32),
        TouchSettingsGroup::Reading);
  addRow(StrId::STR_TOUCH_GROUP_DEVICE, StrId::STR_TOUCH_GROUP_DEVICE_SUMMARY, listIconFor(UIIcon::Info, 32),
        TouchSettingsGroup::DeviceSystem);
}

void TouchSettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  const auto group = static_cast<TouchSettingsGroup>(rowItems[static_cast<size_t>(index)].actionValue);
  startActivityForResult(std::make_unique<TouchSettingsGroupActivity>(renderer, mappedInput, group),
                         [this](const ActivityResult&) { requestUpdate(); });
}

const char* TouchSettingsActivity::headerTitle() const { return tr(STR_TOUCH_SETTINGS_TITLE); }

void TouchSettingsActivity::buildScreen(UiScreen& screen) {
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
  props.rtl = I18N.isRtl();
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

#endif  // FREEINK_CAP_TOUCH
