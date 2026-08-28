#pragma once

#include <functional>

#include "SettingsList.h"

class Activity;
class GfxRenderer;
class MappedInputManager;

// Dispatches a SettingInfo::ACTION entry: launches the matching sub-activity
// (or performs an immediate action, e.g. a silent reboot) via `host`, for the
// touch Settings presentation only. Independently mirrors the WiFi-radio-
// teardown, silent-restart, and two-stage-logout business logic in
// SettingsActivity::toggleCurrentSetting()'s own ACTION switch --
// SettingsActivity.cpp is untouched and does not call this. Every target
// activity launched here is the same pre-existing, button-driven activity
// SettingsActivity opens; sub-screens without their own touch handling still
// work, just via physical buttons once entered. `onRebuildNeeded` is called
// only by the actions that need the caller to refresh its settings list
// afterward (Network, Download Fonts, Fouled eBooks Login/Logout); every
// other action just saves via its own result handler and relies on the
// normal startActivityForResult() -> requestUpdate() flow.
void dispatchSettingAction(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput, SettingAction action,
                           const std::function<void()>& onRebuildNeeded);
