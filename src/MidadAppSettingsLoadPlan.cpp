#include "MidadAppSettingsLoadPlan.h"

MidadAppSettingsLoadPlan planMidadAppSettingsLoad(const bool ownFileExists, const bool ownFileLoadedOk,
                                                  const bool legacyFileExists) {
  if (ownFileExists) {
    // Migrating from legacy settings.json is only ever correct the very
    // first time. A corrupt own file later must NOT fall back to legacy --
    // that would let a torn write on midad_apps.json silently resurrect
    // stale pre-migration values instead of surfacing the failure.
    return ownFileLoadedOk ? MidadAppSettingsLoadPlan::UseOwnFile : MidadAppSettingsLoadPlan::ReportCorrupt;
  }
  return legacyFileExists ? MidadAppSettingsLoadPlan::Migrate : MidadAppSettingsLoadPlan::FreshDefaults;
}
