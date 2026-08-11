#pragma once

#include <vector>

#include "activities/settings/SettingsActivity.h"

class SdCardFontRegistry;

// Midad-owned Apps-screen setting rows: Quran, Games, Tasbih, Stop Watch,
// Pomodoro (+ its three durations), Gym (+ weight unit), Midad BLE, Debug
// Logging. None of these have a CrossPoint upstream equivalent (see
// MidadAppSettings.h, which backs every one of them), so their
// SettingInfo::DynamicToggle/DynamicValue/DynamicEnum row definitions live
// here rather than in the upstream-owned SettingsList.h -- see
// docs/upstream-sync-architecture.md's Phase B for why that split matters
// for merge conflict surface.
//
// SettingsActivity::rebuildSettingsLists() is the one caller: a single hook
// appending this whole family into whatever Apps-category vector it's
// building, in place of what used to be a dozen individual rows mixed into
// the shared getSettingsList() table.
void appendMidadAppSettings(std::vector<SettingInfo>& appsSettings);

// getSettingsList()'s table (src/SettingsList.h) with the Midad Apps rows
// merged in -- the single source of truth CrossPointWebServer's
// /api/settings GET/POST builds from, so the web settings API can't drift
// out of sync with which fields the on-device Settings screen exposes.
//
// SettingsActivity does NOT use this: it needs its own category-then-action
// ordering (Dictionary/KOReader Sync land between the generic list and the
// Midad rows), so it calls getSettingsList() and appendMidadAppSettings()
// separately instead -- see rebuildSettingsLists().
std::vector<SettingInfo> getCombinedSettingsList(const SdCardFontRegistry* registry = nullptr);
