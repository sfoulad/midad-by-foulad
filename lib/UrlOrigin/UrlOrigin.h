#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// scheme://host[:port] prefix, i.e. everything before the first '/', '?', or
// '#' after the scheme separator (a URL with no path but a query/fragment --
// e.g. "https://host?key=value" -- has no '/' at all, so stopping at '/'
// alone would fold the query string into the "origin" and misclassify a
// same-origin redirect as cross-origin). Used to decide whether a redirect
// crosses trust boundaries -- see the Authorization/X-Device-Serial-stripping
// logic in HttpDownloader.cpp's runGet() and runGetWolf().
//
// A view into the caller's own buffer -- allocates nothing. Deliberately so:
// this runs on every redirect hop, including ones immediately before a TLS
// handshake that needs a large contiguous heap block for RSA signature
// verification, and an extra allocation in that exact window is itself a
// fragmentation risk. Never pass the result across a C API boundary (see
// CLAUDE.md's string_view/null-termination rule) -- every call site only
// ever compares two of these with `==`.
inline std::string_view urlOrigin(std::string_view url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return url;
  const size_t pathStart = url.find_first_of("/?#", schemeEnd + 3);
  return pathStart == std::string_view::npos ? url : url.substr(0, pathStart);
}

// urlIsHttps() lives in src/network/HttpVerifiedFetch.h alongside the rest of
// the verified-fetch preconditions.
// The rule "credentials never cross an origin boundary", for a redirect chain --
// written down once, and testable on a host, because getting it wrong hands a
// device token to whoever controls the redirect target.
//
// Construct with the URL the chain starts at, call onHop() with each redirect
// target as the chain advances, and ask credentialsAllowed() before every
// request. It answers for the request that is ABOUT to be sent, so the caller
// must set or clear its Authorization from it on every hop, not only the first.
//
// Latching is the point. Once a chain has left its starting origin the answer is
// no for the remainder of that chain, even if a later hop comes back. A server
// that can redirect can redirect A -> B -> A, and restoring credentials on the
// way back would hand them to whoever chose the timing of that last hop.
//
// "Same origin" is scheme + host + port -- exactly what urlOrigin() returns, not
// the host alone. An https -> http downgrade on the same host is a different
// origin and drops credentials, which is intended. Two spellings of one origin
// that differ textually ("https://h" vs "https://h:443") compare unequal and so
// drop credentials too: the failure direction is an unnecessary strip, never an
// unintended send.
//
// Holds a view into the caller's buffer and allocates nothing (see urlOrigin
// above). The start URL must outlive the guard -- in particular it must not be
// the same variable the caller rewrites as the chain advances.
class RedirectOriginGuard {
 public:
  explicit RedirectOriginGuard(std::string_view startUrl) : startOrigin_(urlOrigin(startUrl)) {}
  // Exact match for a string literal, which would otherwise be ambiguous between
  // the view above and the deleted overload below.
  explicit RedirectOriginGuard(const char* startUrl) : startOrigin_(urlOrigin(startUrl ? startUrl : "")) {}
  // A temporary would leave startOrigin_ dangling on the first comparison.
  explicit RedirectOriginGuard(std::string&&) = delete;

  void onHop(std::string_view url) {
    if (urlOrigin(url) != startOrigin_) leftStartOrigin_ = true;
  }

  bool credentialsAllowed() const { return !leftStartOrigin_; }
  bool leftStartOrigin() const { return leftStartOrigin_; }

 private:
  std::string_view startOrigin_;
  bool leftStartOrigin_ = false;
};

// --- redirect admission control ----------------------------------------------
//
// What this firmware will and will not follow. A redirect is a server telling
// the device to go somewhere else with the request it was already willing to
// make, so every one of these is a "refuse" rather than a "sanitise": there is
// no legitimate cover, feed or download link on this device's catalogues that
// needs any of the shapes below, and silently repairing one would hide a server
// that has been compromised or misconfigured.
//
// Cheap ASCII only, no allocation, no <cctype> (locale-dependent) -- these run
// on the OPDS redirect path, next to a TLS handshake that wants contiguous heap.

enum class RedirectVerdict : uint8_t {
  Follow = 0,
  RejectEmptyLocation,      // absent, empty or whitespace-only Location
  RejectProtocolRelative,   // "//host/..." -- resolves differently per RFC than this SDK does
  RejectUnsupportedScheme,  // anything but http/https: file:, data:, javascript:, mailto:, "http:/x"
  RejectUserinfo,           // "https://user:pass@host/..." -- the host is not what it looks like
  RejectDowngrade,          // https -> http: the body, and anything else on the wire, goes cleartext
};

// Flash-resident literals, safe to log. Never log the URL itself on a rejection:
// a rejected Location is exactly the case that may carry credentials in it.
inline const char* describeRedirectVerdict(const RedirectVerdict verdict) {
  switch (verdict) {
    case RedirectVerdict::Follow:
      return "follow";
    case RedirectVerdict::RejectEmptyLocation:
      return "empty Location";
    case RedirectVerdict::RejectProtocolRelative:
      return "protocol-relative Location";
    case RedirectVerdict::RejectUnsupportedScheme:
      return "unsupported scheme";
    case RedirectVerdict::RejectUserinfo:
      return "userinfo in authority";
    case RedirectVerdict::RejectDowngrade:
      return "https to http downgrade";
  }
  return "unknown";
}

inline bool schemeIs(std::string_view scheme, std::string_view lowercaseName) {
  if (scheme.size() != lowercaseName.size()) return false;
  for (size_t i = 0; i < scheme.size(); ++i) {
    char c = scheme[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != lowercaseName[i]) return false;
  }
  return true;
}

// Lowercase-comparable scheme text before "://", empty when there is no "://".
inline std::string_view urlScheme(std::string_view url) {
  const size_t schemeEnd = url.find("://");
  return schemeEnd == std::string_view::npos ? std::string_view{} : url.substr(0, schemeEnd);
}

// The authority component: everything between "://" and the first '/', '?' or '#'.
inline std::string_view urlAuthority(std::string_view url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return {};
  const size_t start = schemeEnd + 3;
  const size_t end = url.find_first_of("/?#", start);
  return end == std::string_view::npos ? url.substr(start) : url.substr(start, end - start);
}

inline std::string_view trimAsciiSpace(std::string_view text) {
  const auto isSpace = [](const char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
  };
  while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
  return text;
}

// A URL this firmware is willing to send a request to at all, redirect or not.
//
// Userinfo is the reason this exists. "https://api.midad.one@evil.example/" is a
// request to evil.example that reads like one to api.midad.one, and the two URL
// parsers in this codebase do not agree on it: isFouladEbooksUrl() reads the host
// as evil.example (test/foulad_core_endpoints) while SecureHttpClient's parseUrl
// folds the whole "api.midad.one@evil.example" into its host field. Rejecting the
// shape outright is the only answer that does not depend on which parser a given
// call site happens to consult.
inline RedirectVerdict checkRequestUrl(const std::string_view url) {
  const std::string_view scheme = urlScheme(url);
  if (!schemeIs(scheme, "http") && !schemeIs(scheme, "https")) return RedirectVerdict::RejectUnsupportedScheme;
  const std::string_view authority = urlAuthority(url);
  if (authority.empty()) return RedirectVerdict::RejectUnsupportedScheme;
  if (authority.find('@') != std::string_view::npos) return RedirectVerdict::RejectUserinfo;
  return RedirectVerdict::Follow;
}

// The raw Location header, checked BEFORE it is resolved against the current URL,
// because resolving rewrites the shapes that need rejecting: SecureHttpClient's
// resolveUrl() treats anything without "://" as a relative reference, so
// "javascript:alert(1)" would come back as a path under the current host and
// "//evil.example/x" as a double-slashed path rather than the network-path
// reference RFC 3986 says it is. Neither is a thing to repair.
inline RedirectVerdict checkRedirectLocation(const std::string_view location) {
  const std::string_view target = trimAsciiSpace(location);
  if (target.empty()) return RedirectVerdict::RejectEmptyLocation;
  if (target.size() >= 2 && target[0] == '/' && target[1] == '/') return RedirectVerdict::RejectProtocolRelative;

  // A ':' before any '/', '?' or '#' means this is an absolute URI with a scheme --
  // a relative reference's first segment cannot contain one (RFC 3986 3.3).
  const size_t firstDelimiter = target.find_first_of(":/?#");
  if (firstDelimiter != std::string_view::npos && target[firstDelimiter] == ':') {
    if (!schemeIs(target.substr(0, firstDelimiter), "http") && !schemeIs(target.substr(0, firstDelimiter), "https")) {
      return RedirectVerdict::RejectUnsupportedScheme;
    }
    // http/https must carry an authority: "http:/host/x" and "https:x" are not
    // things a real server sends, and resolveUrl() would mangle both.
    if (target.substr(firstDelimiter).rfind("://", 0) != 0) return RedirectVerdict::RejectUnsupportedScheme;
    return checkRequestUrl(target);
  }
  return RedirectVerdict::Follow;  // ordinary relative reference
}

// The resolved redirect target, checked before the chain advances to it.
//
// The downgrade rule is deliberately about the TRANSITION, not the target: an
// http -> http chain is a plain-HTTP server that was never confidential and has
// nothing to strip (local and self-hosted OPDS servers are legitimately http, and
// so is FOULAD_EBOOKS_NEWS_URL), while https -> http is a server discarding
// transport security mid-request -- the classic stripping shape, and it would put
// the response body, this device's cover images included, on the wire in clear.
inline RedirectVerdict checkRedirectTarget(const std::string_view previousUrl, const std::string_view resolvedUrl) {
  const RedirectVerdict targetVerdict = checkRequestUrl(resolvedUrl);
  if (targetVerdict != RedirectVerdict::Follow) return targetVerdict;
  if (schemeIs(urlScheme(previousUrl), "https") && schemeIs(urlScheme(resolvedUrl), "http")) {
    return RedirectVerdict::RejectDowngrade;
  }
  return RedirectVerdict::Follow;
}
