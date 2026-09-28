#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Splits an absolute http/https URL into the pieces a TLS client needs to keep
// straight: the bare hostname it must connect to AND require the certificate to
// name, the port, and the request target.
//
// Its reason to exist is the hostname: everything downstream (SNI, the Host
// header, wolfSSL's domain-name check) has to be handed the SAME name, and it
// must be the authority's real host -- not a prefix an attacker chose. A URL
// like "https://api.midad.one@evil.example/x" is a request to evil.example, and
// a parser that stops at the first '@' or at the first '/' would hand the
// verifier "api.midad.one" while the socket talks to evil.example. Userinfo is
// therefore stripped here, in the one place every consumer reads.
//
// Header-only and free of Arduino/ESP-IDF types so the host tests exercise the
// exact code the firmware links (see test/http_url_parts).
struct HttpUrlParts {
  std::string host;    // bare hostname: userinfo, port and IPv6 brackets removed
  std::string path;    // path + query, always starts with '/'
  uint16_t port = 0;   // explicit port, else the scheme default
  bool https = false;  // false: plain http
};

namespace http_url_parts_detail {

inline bool iequalsAscii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char lhs = a[i];
    char rhs = b[i];
    if (lhs >= 'A' && lhs <= 'Z') lhs = static_cast<char>(lhs - 'A' + 'a');
    if (rhs >= 'A' && rhs <= 'Z') rhs = static_cast<char>(rhs - 'A' + 'a');
    if (lhs != rhs) return false;
  }
  return true;
}

// Hostnames only: no delimiter may survive into the name handed to the
// certificate check, and an empty label set is not a host.
inline bool isPlausibleHost(std::string_view host) {
  if (host.empty()) return false;
  for (const char c : host) {
    if (c == '/' || c == '?' || c == '#' || c == '@' || c == ':' || c == '\\' || c <= ' ') return false;
  }
  return true;
}

}  // namespace http_url_parts_detail

inline bool parseHttpUrl(std::string_view url, HttpUrlParts& out) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return false;
  const std::string_view scheme = url.substr(0, schemeEnd);

  bool https = false;
  if (http_url_parts_detail::iequalsAscii(scheme, "https"))
    https = true;
  else if (!http_url_parts_detail::iequalsAscii(scheme, "http"))
    return false;

  std::string_view rest = url.substr(schemeEnd + 3);
  // The authority ends at the first '/', '?' or '#': a URL with a query but no
  // path ("https://host?a=b") has no '/' at all.
  const size_t authorityEnd = rest.find_first_of("/?#");
  std::string_view authority = authorityEnd == std::string_view::npos ? rest : rest.substr(0, authorityEnd);
  std::string_view target = authorityEnd == std::string_view::npos ? std::string_view() : rest.substr(authorityEnd);

  // Userinfo ("user:pass@host"): the host is what follows the LAST '@'.
  const size_t at = authority.rfind('@');
  if (at != std::string_view::npos) authority.remove_prefix(at + 1);

  std::string_view host = authority;
  std::string_view portText;
  if (!host.empty() && host.front() == '[') {  // IPv6 literal
    const size_t close = host.find(']');
    if (close == std::string_view::npos) return false;
    const std::string_view after = host.substr(close + 1);
    if (!after.empty() && after.front() != ':') return false;
    if (!after.empty()) portText = after.substr(1);
    host = host.substr(1, close - 1);
    if (host.empty()) return false;
  } else {
    const size_t colon = host.find(':');
    if (colon != std::string_view::npos) {
      portText = host.substr(colon + 1);
      host = host.substr(0, colon);
    }
    if (!http_url_parts_detail::isPlausibleHost(host)) return false;
  }

  uint32_t port = https ? 443u : 80u;
  if (!portText.empty()) {
    port = 0;
    for (const char c : portText) {
      if (c < '0' || c > '9') return false;
      port = port * 10 + static_cast<uint32_t>(c - '0');
      if (port > 65535) return false;
    }
    if (port == 0) return false;
  }

  out.https = https;
  out.host.assign(host);
  out.port = static_cast<uint16_t>(port);
  // "https://host?a=b" has a query and no path; the request target still needs one.
  if (target.empty())
    out.path = "/";
  else if (target.front() == '/')
    out.path.assign(target);
  else
    out.path = std::string("/") + std::string(target);
  return true;
}
