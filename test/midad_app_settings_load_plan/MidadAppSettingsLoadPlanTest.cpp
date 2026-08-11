#include <gtest/gtest.h>

#include "MidadAppSettingsLoadPlan.h"

// The exact distinction MidadAppSettings::loadFromFile() must preserve: a
// missing own file is a normal, expected migration trigger, but a corrupt
// own file must never fall back to legacy settings.json -- see
// MidadAppSettingsLoadPlan.h's comment for why.

TEST(MidadAppSettingsLoadPlan, OwnFileLoadsSuccessfully) {
  EXPECT_EQ(planMidadAppSettingsLoad(/*ownFileExists=*/true, /*ownFileLoadedOk=*/true, /*legacyFileExists=*/true),
            MidadAppSettingsLoadPlan::UseOwnFile);
  EXPECT_EQ(planMidadAppSettingsLoad(/*ownFileExists=*/true, /*ownFileLoadedOk=*/true, /*legacyFileExists=*/false),
            MidadAppSettingsLoadPlan::UseOwnFile);
}

TEST(MidadAppSettingsLoadPlan, OwnFileExistsButCorruptDoesNotMigrate) {
  // The regression this test guards against: treating "own file present but
  // failed to parse" the same as "own file absent" would silently resurrect
  // stale legacy values behind a torn write, regardless of whether a legacy
  // file happens to exist.
  EXPECT_EQ(planMidadAppSettingsLoad(/*ownFileExists=*/true, /*ownFileLoadedOk=*/false, /*legacyFileExists=*/true),
            MidadAppSettingsLoadPlan::ReportCorrupt);
  EXPECT_EQ(planMidadAppSettingsLoad(/*ownFileExists=*/true, /*ownFileLoadedOk=*/false, /*legacyFileExists=*/false),
            MidadAppSettingsLoadPlan::ReportCorrupt);
}

TEST(MidadAppSettingsLoadPlan, MissingOwnFileWithLegacyMigrates) {
  EXPECT_EQ(planMidadAppSettingsLoad(/*ownFileExists=*/false, /*ownFileLoadedOk=*/false, /*legacyFileExists=*/true),
            MidadAppSettingsLoadPlan::Migrate);
}

TEST(MidadAppSettingsLoadPlan, MissingOwnFileNoLegacyUsesDefaults) {
  EXPECT_EQ(planMidadAppSettingsLoad(/*ownFileExists=*/false, /*ownFileLoadedOk=*/false, /*legacyFileExists=*/false),
            MidadAppSettingsLoadPlan::FreshDefaults);
}
