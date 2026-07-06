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
