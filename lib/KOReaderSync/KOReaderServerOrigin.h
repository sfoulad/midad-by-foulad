#pragma once

#include <string_view>

// Whether a configured KOSync server URL points at a Midad origin.
//
// Midad does not implement the KOSync protocol at all: account-scoped resume
// sync lives on api.midad.one/resume/v1/items/:id/position (Core's supported
// contract, already used by FouladReadingPosition), and the pre-rename hosts are
// forbidden origins outright. KOReaderSyncClient refuses these rather than
// sending anything, because it authenticates with a username and an unsalted MD5
// of the password over a connection it does not verify.
//
// Header-only and free of Arduino dependencies so the host test suite exercises
// the identical code the firmware runs -- a second implementation of "is this a
// Midad host" is exactly how such a check comes to disagree with itself.
//
// Hostnames are duplicated from src/FouladEbooksConfig.h, which stays the source
// of truth; lib/ is not on an -Isrc include path in every build environment.
namespace koreader_sync {

inline bool isMidadSyncOrigin(std::string_view url) {
  const auto schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return false;
  url.remove_prefix(schemeEnd + 3);

  // Authority runs to the first '/', '?' or '#'.
  const auto authorityEnd = url.find_first_of("/?#");
  std::string_view host = authorityEnd == std::string_view::npos ? url : url.substr(0, authorityEnd);

  // Strip userinfo ("user:pass@host") and port, leaving the bare hostname, so
  // "https://user:pass@midad.one:443/x" still compares as "midad.one".
  const auto at = host.rfind('@');
  if (at != std::string_view::npos) host.remove_prefix(at + 1);
  const auto colon = host.find(':');
  if (colon != std::string_view::npos) host = host.substr(0, colon);

  // Whole-host, case-insensitive comparison -- never a substring match, or
  // "midad.one.attacker.tld" and "notmidad.one" would both be treated as Midad.
  const auto hostEquals = [](std::string_view h, std::string_view expected) {
    if (h.size() != expected.size()) return false;
    for (size_t i = 0; i < h.size(); ++i) {
      const char c = h[i] >= 'A' && h[i] <= 'Z' ? static_cast<char>(h[i] - 'A' + 'a') : h[i];
      if (c != expected[i]) return false;
    }
    return true;
  };

  return hostEquals(host, "midad.one") || hostEquals(host, "api.midad.one") || hostEquals(host, "foulad.one");
}

}  // namespace koreader_sync
