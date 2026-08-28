#include "TouchSettingsGroupActivity.h"

#if FREEINK_CAP_TOUCH

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>

#include "MappedInputManager.h"
#include "MidadTouchSettingsSupport.h"
#include "SettingsActionDispatch.h"
#include "activities/util/FrontlightPanelActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/TouchOptionPickerActivity.h"
#include "components/UITheme.h"
#include "components/icons/listIcons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

TouchSettingsGroupActivity::TouchSettingsGroupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       const TouchSettingsGroup group)
    : UiListActivity("TouchSettingsGroup", renderer, mappedInput), group(group) {}

void TouchSettingsGroupActivity::onEnter() {
  UiListActivity::onEnter();
  RenderLock lock(*this);
  settings = buildTouchSettingsGroups()[group];
  rebuildRowItems();
}

void TouchSettingsGroupActivity::onExit() {
  SETTINGS.saveToFile();
  UiListActivity::onExit();
}

const char* TouchSettingsGroupActivity::headerTitle() const {
  switch (group) {
    case TouchSettingsGroup::General:
      return tr(STR_TOUCH_GROUP_GENERAL);
    case TouchSettingsGroup::DisplayLighting:
      return tr(STR_TOUCH_GROUP_DISPLAY);
    case TouchSettingsGroup::NetworkBluetooth:
      return tr(STR_TOUCH_GROUP_NETWORK);
    case TouchSettingsGroup::Reading:
      return tr(STR_TOUCH_GROUP_READING);
    case TouchSettingsGroup::DeviceSystem:
    default:
      return tr(STR_TOUCH_GROUP_DEVICE);
  }
}

// TOGGLE -> a switch (ListItem::toggle); everything else -> a right-aligned
// value string, persisted per-row in rowValueStrings since ListItem::value is
// a non-owning const char*. ACTION rows get a plain ">" affordance (mirrored
// under RTL) since the generic list() renderer has no chevron slot of its own
// -- see components/lists/list.h's ListItem/ListProps.
void TouchSettingsGroupActivity::rebuildRowItems() {
  rowItems.clear();
  rowValueStrings.clear();
  rowItems.reserve(settings.size());
  rowValueStrings.reserve(settings.size());

  const bool rtl = I18N.isRtl();
  for (size_t i = 0; i < settings.size(); i++) {
    const auto& setting = settings[i];
    fui::ListItem item;
    item.label = I18N.get(setting.nameId);
    item.actionValue = static_cast<int16_t>(i);

    if (setting.type == SettingType::TOGGLE) {
      const bool checked = setting.valuePtr != nullptr ? (SETTINGS.*(setting.valuePtr) != 0)
                                                       : (setting.valueGetter && setting.valueGetter() != 0);
      item.toggle = true;
      item.toggleChecked = checked;
      rowValueStrings.emplace_back();
    } else if (setting.type == SettingType::ACTION) {
      rowValueStrings.push_back(rtl ? "<" : ">");
      item.value = rowValueStrings.back().c_str();
    } else {
      rowValueStrings.push_back(formatSettingValueText(setting));
      item.value = rowValueStrings.back().c_str();
    }
    rowItems.push_back(item);
  }

  hasFrontlightRow = group == TouchSettingsGroup::DisplayLighting;
  if (hasFrontlightRow) {
    fui::ListItem item;
    item.label = I18N.get(StrId::STR_FRONTLIGHT);
    item.icon = fui::bitmapFromIcon(icon_sun_32);
    item.actionValue = static_cast<int16_t>(settings.size());
    rowValueStrings.push_back(rtl ? "<" : ">");
    item.value = rowValueStrings.back().c_str();
    rowItems.push_back(item);
  }
}

void TouchSettingsGroupActivity::openFrontlightPanel() {
  startActivityForResult(std::make_unique<FrontlightPanelActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void TouchSettingsGroupActivity::editEnum(const int index) {
  const auto& setting = settings[static_cast<size_t>(index)];
  std::vector<std::string> options;
  int current = 0;
  if (setting.valuePtr != nullptr) {
    current = SETTINGS.*(setting.valuePtr);
    options.reserve(setting.enumValues.size());
    for (const auto strId : setting.enumValues) options.emplace_back(I18N.get(strId));
  } else if (setting.valueGetter) {
    current = setting.valueGetter();
    if (!setting.enumStringValues.empty()) {
      options = setting.enumStringValues;
    } else {
      options.reserve(setting.enumValues.size());
      for (const auto strId : setting.enumValues) options.emplace_back(I18N.get(strId));
    }
  }

  startActivityForResult(std::make_unique<TouchOptionPickerActivity>(renderer, mappedInput, I18N.get(setting.nameId),
                                                                     std::move(options), current),
                         [this, index](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             if (const auto* picked = std::get_if<IntervalResult>(&result.data)) {
                               auto& picked_setting = settings[static_cast<size_t>(index)];
                               const auto value = static_cast<uint8_t>(picked->value);
                               if (picked_setting.valuePtr != nullptr) {
                                 SETTINGS.*(picked_setting.valuePtr) = value;
                               } else if (picked_setting.valueSetter) {
                                 picked_setting.valueSetter(value);
                               }
                               SETTINGS.saveToFile();
                             }
                           }
                           rebuildRowItems();
                           requestUpdate();
                         });
}

void TouchSettingsGroupActivity::editValue(const int index) {
  const auto& setting = settings[static_cast<size_t>(index)];
  const int current =
      setting.valuePtr != nullptr ? SETTINGS.*(setting.valuePtr) : (setting.valueGetter ? setting.valueGetter() : 0);

  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(renderer, mappedInput, "TouchSettingsValue", setting.nameId, current,
                                                  setting.valueRange.min, setting.valueRange.max,
                                                  setting.valueRange.step, setting.valueRange.step),
      [this, index](const ActivityResult& result) {
        if (!result.isCancelled) {
          if (const auto* picked = std::get_if<IntervalResult>(&result.data)) {
            auto& picked_setting = settings[static_cast<size_t>(index)];
            const auto value = static_cast<uint8_t>(picked->value);
            if (picked_setting.valuePtr != nullptr) {
              SETTINGS.*(picked_setting.valuePtr) = value;
            } else if (picked_setting.valueSetter) {
              picked_setting.valueSetter(value);
            }
            SETTINGS.saveToFile();
          }
        }
        rebuildRowItems();
        requestUpdate();
      });
}

void TouchSettingsGroupActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(rowItems.size())) return;
  app.clearTapFlash();
  nav.selected = index;

  if (hasFrontlightRow && index == static_cast<int>(settings.size())) {
    openFrontlightPanel();
    return;
  }

  auto& setting = settings[static_cast<size_t>(index)];

  if (setting.type == SettingType::TOGGLE) {
    applySettingToggle(setting, renderer);
    SETTINGS.saveToFile();
    rebuildRowItems();
    requestUpdate(true);
  } else if (setting.type == SettingType::ENUM) {
    editEnum(index);
  } else if (setting.type == SettingType::VALUE) {
    editValue(index);
  } else if (setting.type == SettingType::ACTION) {
    dispatchSettingAction(*this, renderer, mappedInput, setting.action, [this] {
      rebuildRowItems();
      requestUpdate();
    });
  }
  // STRING-type settings (KOReader credentials) live inside
  // KOReaderSettingsActivity, not folded into a top-level group's row list --
  // nothing to handle here.
}

void TouchSettingsGroupActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (rowItems.empty()) {
    screen.centeredText(tr(STR_NONE_OPT), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props, /*hasSubtitle=*/false);
  screen.list(props);
}

#endif  // FREEINK_CAP_TOUCH
