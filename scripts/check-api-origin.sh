#!/usr/bin/env bash
# check-api-origin.sh [--binaries]
#
# Fails closed if firmware source or a built board binary references any Midad
# application API origin other than https://api.midad.one.
#
# HOST-AWARE, NOT SUBSTRING. "api.midad.one" contains "midad.one", so a
# substring test either passes everything or fails everything. Every URL is
# parsed down to its authority and compared as a whole host.
set -uo pipefail
cd "$(dirname "$0")/.."

ALLOWED_API_HOST="api.midad.one"
# Tightly scoped, per the architecture directive. Neither is an alternative
# Midad application API.
ALLOWED_OTHER_HOSTS=(
  "id.midad.one"                    # identity / OIDC
  "github.com" "api.github.com" "raw.githubusercontent.com"
  "objects.githubusercontent.com" "release-assets.githubusercontent.com"
  "gist.github.com"
  "sync.koreader.rocks"             # user-configured KOReader sync
)
FORBIDDEN_MIDAD_HOSTS=("midad.one" "www.midad.one")

host_of() {                         # strip scheme, then anything from / ? # :
  local u="${1#*://}"; u="${u%%[/?#]*}"; u="${u%%:*}"; printf '%s' "$u"
}
# Substring is used ONLY to SELECT a host for checking, never to decide whether
# it is allowed -- that decision is always an exact whole-host comparison. Any
# host whose name contains "midad.one" anywhere must match the allow-list
# exactly, so a spoof like api.midad.one.attacker.tld is caught rather than
# mistaken for a non-Midad host and ignored.
is_midad_host() { case "$1" in *midad.one*) return 0;; *) return 1;; esac; }

fail=0
report() { printf '  %-22s %s\n' "$1" "$2"; }

# ---- source scan: executable lines only, comments are allowed to be historical
echo "== source =="
while IFS= read -r hit; do
  file="${hit%%:*}"; rest="${hit#*:}"; line="${rest%%:*}"; text="${rest#*:}"
  # skip comment-only lines (// or * or #) -- historical mentions are permitted
  stripped="$(printf '%s' "$text" | sed 's/^[[:space:]]*//')"
  # Only // and /* ... */ are comments. "#" is a preprocessor directive in C++,
  # NOT a comment -- treating it as one made #define lines invisible to this guard.
  case "$stripped" in //*|\**|/\**) continue;; esac
  url="$(printf '%s' "$text" | grep -oE 'https?://[A-Za-z0-9._-]+' | head -1)"
  [ -z "$url" ] && continue
  h="$(host_of "$url")"
  is_midad_host "$h" || continue
  [ "$h" = "$ALLOWED_API_HOST" ] && continue
  for a in "${ALLOWED_OTHER_HOSTS[@]}"; do [ "$h" = "$a" ] && continue 2; done
  report "FORBIDDEN $h" "$file:$line"
  fail=1
done < <(grep -rnE 'https?://[A-Za-z0-9._-]*midad\.one' src lib 2>/dev/null)
[ "$fail" -eq 0 ] && echo "  ok -- no forbidden Midad API origin in executable source"

# ---- binary scan
if [ "${1:-}" = "--binaries" ]; then
  echo "== binaries =="
  found_any=0
  while IFS= read -r bin; do
    found_any=1
    bad=0
    while IFS= read -r url; do
      h="$(host_of "$url")"
      is_midad_host "$h" || continue
      [ "$h" = "$ALLOWED_API_HOST" ] && continue
      for a in "${ALLOWED_OTHER_HOSTS[@]}"; do [ "$h" = "$a" ] && continue 2; done
      report "FORBIDDEN $h" "$bin"
      bad=1; fail=1
    done < <(strings "$bin" | grep -oE 'https?://[A-Za-z0-9._-]*midad\.one[A-Za-z0-9._/-]*' | sort -u)
    [ "$bad" -eq 0 ] && report "ok" "$bin"
  done < <(find .pio/build -name firmware.bin 2>/dev/null | sort)
  [ "$found_any" -eq 0 ] && echo "  (no built binaries found -- build first)"
fi

if [ "$fail" -ne 0 ]; then
  echo "FAIL: a forbidden Midad API origin is present. Only https://api.midad.one is permitted."
  exit 1
fi
echo "PASS: api.midad.one is the only Midad application API origin."
