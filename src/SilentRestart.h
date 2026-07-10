#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)
// OTA update entry points, landing straight back in OtaUpdateActivity on a
// fresh boot. Device reports showed BOTH the version-check GET and the
// firmware-download TLS handshake failing with a recurring ~35KB
// largest-contiguous-block ceiling even with 50-75KB free in total --
// whatever fragmented the heap during Home/library browsing before OTA was
// even opened can't be un-fragmented by freeing this session's own memory.
// Rebooting first gives every TLS handshake in the flow a heap with nothing
// else having allocated anything yet.
void silentRestartToOtaCheck();    // start of the flow (Home/Settings entry)
void silentRestartToOtaInstall();  // user already confirmed; auto-install armed
