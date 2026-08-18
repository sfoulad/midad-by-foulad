// esp_secure_boot_init_checks() (esp-idf bootloader_support/src/secure_boot.c)
// runs on every single boot via ESP_SYSTEM_INIT_FN(init_efuse, CORE, BIT(0), 140)
// in components/efuse/src/esp_efuse_startup.c -- not just at OTA-install time,
// despite CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT's own Kconfig help text
// (and this project's original PR #156) both describing it as install-time-only.
// With that option set (platformio.ini [base] custom_sdkconfig, applied to every
// env), if the *running* app carries zero signature blocks it calls abort()
// immediately, then again on every subsequent boot -- confirmed boot-looping a
// real X3 the moment an unsigned dev build was flashed
// (FE-P3-OTA-SIGNING-CONFIG-001).
//
// Giving dev/diagnostic envs a different custom_sdkconfig than release envs (the
// seemingly obvious fix) isn't safe here: pioarduino's Arduino-core build cache
// is keyed by board, not by env, so two same-board envs with differing
// custom_sdkconfig content corrupt each other's cached custom_component_remove
// state (confirmed empirically -- esp_insights/rainmaker silently stopped being
// excluded from an unrelated env's build). This wraps the boot-time check itself
// instead, via -Wl,--wrap=esp_secure_boot_init_checks (platformio.ini [base]
// build_flags, identical for every env) gated by a build_flag that IS safe to
// vary per env: OTA_SIGNING_BOOT_CHECK_ENABLED, defined only for
// gh_release/gh_release_rc/sticky-gh_release/sticky-gh_release_rc. Every env
// keeps a byte-identical custom_sdkconfig and the underlying signature-
// verification code (esp_ota_ops.c) stays compiled in and unchanged for
// everyone -- this only skips the boot-time self-check for envs that, by
// design, never carry a production signature.
#include <esp_secure_boot.h>

extern "C" void __real_esp_secure_boot_init_checks(void);

extern "C" void __wrap_esp_secure_boot_init_checks(void) {
#ifdef OTA_SIGNING_BOOT_CHECK_ENABLED
  __real_esp_secure_boot_init_checks();
#endif
}
