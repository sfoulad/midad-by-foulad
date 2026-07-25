#pragma once

// Non-secret catalog identity for the built-in "Foulad eBooks" home menu entry.
// Username/password are entered on-device on first use (FouladEbooksSetupActivity)
// and stored via OpdsServerStore — never hardcoded here, since this repo is public.
constexpr char FOULAD_EBOOKS_NAME[] = "Foulad eBooks";
// Temporarily http:// (was https://), during beta only: Let's Encrypt rotated
// foulad.one onto a certificate hierarchy ("ISRG Root YE") that ESP-IDF's embedded
// trust bundle doesn't recognize, and the on-device fallback that could otherwise
// verify it was removed after it drove free heap down to ~5KB and froze the device
// (see HttpDownloader.cpp). The server has been reconfigured to serve OPDS over
// plain HTTP without redirecting to HTTPS, specifically so this can keep working
// during beta testing until the certificate chain is fixed server-side or ESP-IDF
// updates its bundle. Basic Auth credentials travel in cleartext over this
// connection as a result -- an accepted, explicit tradeoff for beta, not something
// to carry into a production release without revisiting.
constexpr char FOULAD_EBOOKS_URL[] = "http://foulad.one/opds";

// Font-conversion relay endpoint (Settings/File Transfer portal -> Fonts ->
// Convert a Font): the device uploads a raw TTF/OTF plus a language choice,
// foulad-ebooks converts it into the device's .cpfont SD-font format (4 sizes)
// and responds with download URLs. Open endpoint, no auth -- foulad-ebooks is
// expected to rate-limit this server-side. Plain http:// for the same reason
// as FOULAD_EBOOKS_URL above (foulad.one's current certificate chain isn't in
// ESP-IDF's trust bundle); revisit alongside that fix.
constexpr char FOULAD_EBOOKS_FONT_CONVERT_URL[] = "http://foulad.one/api/fonts/convert";

// QR-code sign-in (see FouladDeviceLogin.h). Unauthenticated JSON POSTs, no
// cookies/CSRF. Plain http:// is deliberate and expected here for the same
// certificate-chain reason as FOULAD_EBOOKS_URL above -- these two endpoints sit
// outside the server's HTTPS-forced group precisely so the device can reach them.
// Nothing secret is sent TO these endpoints; the token they return is what
// replaces the account password on the wire, so this flow strictly reduces what
// is exposed rather than adding to it.
constexpr char FOULAD_EBOOKS_DEVICE_LOGIN_START_URL[] = "http://foulad.one/api/device-login/start";
constexpr char FOULAD_EBOOKS_DEVICE_LOGIN_POLL_URL[] = "http://foulad.one/api/device-login/poll";
