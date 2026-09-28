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

// File Transfer (web server), same rationale as OTA: the WiFi driver plus the
// web server's TCP buffers want large contiguous allocations, and on a
// fragmented heap page loads slow to a crawl. The exit path already
// silentRestart()s (CrossPointWebServerActivity::onExit); this makes the entry
// path symmetric.
void silentRestartToFileTransfer();

// Foulad eBooks (OPDS browser), same rationale: WiFi + feed parsing + cover
// download/decode stack up allocations, and browsing started from a fragmented
// session heap ended in an OOM abort() twice on-device (crash reports 7 and 8,
// heap sliding 36KB -> 14KB across cover fetches). The browser's exit path
// already silent-restarts when WiFi was up; this makes entry symmetric.
void silentRestartToFouladEbooks();

// Sign-out, same rationale as the flows above and for a measured reason: the
// self-revoke POST is a TLS handshake, and reached from Settings with WiFi
// already up it found 14,980 bytes free with a 9,716-byte largest block on an
// X3 -- against the ~17KB contiguous a handshake needs. The connection could
// not open at all, so sign-out reported a transport failure the user could do
// nothing about. Rebooting first gives the handshake an unfragmented heap.
void silentRestartToLogout();

// Pairing (FouladQrLoginActivity), reached from the phone over BLE
// (account.register). The reboot is what frees NimBLE's ~57KB and lets WiFi come
// up for the device-login calls; the pairing code is still generated on and
// displayed by the reader, never carried over BLE.
void silentRestartToPairing();

// Apps menu, for the Bluetooth screen's return leg -- see silentRestartToBluetooth()
// and BluetoothActivity::onExit().
void silentRestartToApps();

// Back to the Bluetooth screen once a WiFi service (file transfer / settings)
// is closed from the phone, so the user can pick another service. BLE and WiFi
// cannot be up together on the C3, so this is the return leg of the handover.
void silentRestartToBluetooth();

// Return-to-caller targets: a WiFi flow launched FROM this menu should land
// back on it, not on Home, once its own onExit() silent-restarts to clear
// heap fragmentation. Landing on Home instead of the caller was a real user
// complaint -- Gym catalog/asset sync, the font download store, and the
// dictionary download store all used the bare silentRestart() (home-only)
// with no way back to where the user actually was.
void silentRestartToGym();         // GymActivity (catalog/asset sync callers)
void silentRestartToSettings();    // SettingsActivity (font download caller)
void silentRestartToDictionary();  // DictionaryActivity (dictionary download caller)

// True when the current boot was produced by any silentRestart* call -- i.e.
// the heap is as fresh as a reboot can make it. Used as a loop guard by
// callers that trigger a silent restart to escape heap fragmentation: if the
// fresh boot STILL can't satisfy them, rebooting again won't help.
bool bootWasSilentRestart();

// Reboots immediately after an activity releases exclusive raw storage. The
// RTC target ensures setup() lands on Home instead of resuming a reader.
void restartToHomeAfterStorageHandoff();
