#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)
// A confirmed OTA update, landing straight back in OtaUpdateActivity with
// auto-install armed. A device report showed the firmware-download TLS
// handshake failing (INTERNAL_UPDATE_ERROR) with as little as ~36KB of
// *contiguous* free heap even though tens of KB were free in total --
// whatever fragmented the heap during Home/library browsing before OTA was
// even opened can't be un-fragmented by freeing more of THIS session's own
// memory. Rebooting right after the user confirms, before the download ever
// starts, gives esp_https_ota_begin() a heap with nothing else having
// allocated anything yet.
void silentRestartToOtaInstall();
