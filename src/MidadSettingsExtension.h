#pragma once

#include <vector>

#include "activities/settings/SettingsTypes.h"

// Midad's Settings contribution, expressed entirely through CrossPoint's
// generic extension point (activities/settings/SettingsExtension.h). Returns
// the "Apps" and "More" tabs appended after CrossPoint's built-in four; every
// Midad row and action lives here or in MidadSettingsList.cpp, so
// SettingsActivity.cpp stays byte-identical to upstream.
//
// SettingsActivity calls this once per rebuild (screen entry, a setting
// change, a language switch, and after any extension action), so rows whose
// state changes -- the Foulad eBooks Login/Logout slot, the Arabic font's
// inline value, the frontlight row's presence guard -- stay current without
// extra plumbing.
std::vector<SettingsExtensionCategory> midadSettingsExtensionProvider();

// Installs the provider. Called once from setup() in src/main.cpp, before the
// first activity is entered.
void registerMidadSettingsExtension();
