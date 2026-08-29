# Midad Core OPDS integration: readiness review (prep only, nothing wired up)

Status: **research and recommendation only, like `docs/tls-trust-architecture.md`.
Nothing in this document is implemented or activated.** Written to prepare
firmware-side readiness for pointing the built-in "Foulad eBooks"/Midad catalog
entry at Midad Core (`midad-core-by-foulad`) instead of the legacy Laravel
backend, once Core is actually ready to serve real device traffic. Target host:
`https://api.midad.one` (see `src/FouladCoreConfig.h`, added alongside this
doc -- an inert, unincluded header, not part of any build).

Findings below come from three sources: reading `midad-core-by-foulad`'s Round 8
OPDS commit (`34ca81c`, `apps/api/src/opds/routes.ts`) directly; reading this
repo's own `HttpDownloader.cpp`/`FouladEbooksConfig.h`/`OpdsParser.cpp`/
`docs/opds-server-reference.md`/`docs/tls-trust-architecture.md`; and live
`curl`/`openssl s_client` checks against `https://api.midad.one` (reachable and
serving real Round 8 responses as of this writing).

## 1. OPDS contract review: Core (Round 8) vs. this device's assumptions

Core's `apps/api/src/opds/routes.ts` is mounted at `/opds` (root resolves at the
bare `/opds`, not `/opds/` -- a Hono sub-app quirk; `/opds/` 301s to `/opds`,
confirmed live). Live-checked:

```
$ curl -I https://api.midad.one/opds
HTTP/2 200
content-type: application/atom+xml;profile=opds-catalog;kind=navigation;charset=utf-8
server: cloudflare
```

Root feed body matches the expected Atom/OPDS navigation shape (`<link
rel="subsection">` entries for "All Books" + each Store category), structurally
compatible with what `lib/OpdsParser/` already parses.

**Real mismatches found, none of them XML-shape bugs -- all either auth or
content-coverage gaps:**

1. **Authentication model is different, not just differently-configured.**
   Legacy protects the entire OPDS surface (browse + download) with HTTP Basic
   Auth, which is the only thing `OpdsServerStore`/`HttpDownloader.cpp` know how
   to send (`username`/`password` -> `Authorization: Basic base64(...)`, see
   `runGet()`/`runGetWolf()`). Core has **no Basic Auth at all** (final owner
   decision recorded in the Round 8 commit: Bearer-JWT-only). Root/category/
   books-listing/search are public and need no auth; only
   `GET /opds/books/{id}/download` requires `Authorization: Bearer <token>`
   (plain 401 without it, before the handler even runs).

   **This device has no code path that produces a Bearer token.** The Round 8
   commit's own docblock says this explicitly and puts it out of scope for
   Round 8: *"Bridging a real XTEINK/KOReader device (which today only speaks
   Basic Auth or a QR-issued device token) onto Bearer auth is explicitly OUT
   of scope here -- it is the one hardware-gated item left for Round 9."* That
   matches what's actually in this firmware: `FouladDeviceLogin`'s QR flow
   yields Basic-Auth-compatible credentials (a per-device token sent as the
   Basic Auth password, see `OpdsServer::isDeviceToken`), and nothing in this
   codebase speaks OAuth/OIDC Authorization Code + PKCE (the flow Core's
   website now uses, per midad-core's Round 8.3 commit) or holds a Bearer
   token anywhere. **Browsing the catalog would work against Core today;
   downloading a book would not**, until Round 9 (or equivalent) ships a
   device-auth bridge on Core's side and this firmware gains a Bearer-token
   code path. Not attempted here -- out of scope for a prep-only branch.

2. **Host-allowlist gap if this were wired up.** `FouladEbooksConfig.h`'s
   `isFouladEbooksUrl()` only recognizes the literal hosts `midad.one` and
   `foulad.one` (exact `hostEquals()`, no subdomain matching) when deciding
   whether to attach `X-Device-Serial`. `api.midad.one` would not match either
   literal as written, so a naive host swap would silently stop sending the
   device-serial header to Core (breaking the eventual remote-removal/
   revocation flow that header exists for) rather than failing loudly. Whoever
   wires this up for real needs to extend that allowlist deliberately, not
   just flip a URL constant.

3. **Download URL shape changed, but transparently to this device.** Legacy
   signs cover/download links as Laravel `?signature=...&user=...` query
   params, 30-day expiry. Core's `/opds/books/{id}/download` 302-redirects to
   a SigV4-presigned Cloudflare R2 URL (`X-Amz-*` query params), 300-second
   expiry (`DOWNLOAD_URL_TTL_SECONDS = 300` in `routes.ts`). This is a real
   shape difference, but per `docs/opds-server-reference.md`'s own note ("the
   device never needs to construct this URL itself, just fetch whatever href
   the feed gives it"), the client already treats these as opaque -- no
   client-side URL-construction logic depends on the Laravel shape. The short
   5-minute TTL only matters if a caller ever tried to cache/replay a download
   URL across a long gap, which nothing in `OpdsBookBrowserActivity.cpp`
   currently does (it fetches, then downloads, in one flow).

## 2. XTC/content-type compatibility item

Searched for prior documentation of an XTC/content-type compatibility concern.
**Found and confirmed** -- this is a real, already-fixed item in this
firmware's own history, not a Core-specific one:

- Commit `01daccde` ("fix: recognize application/x-xtch acquisition links, not
  just x-xtc") -- the legacy OPDS server derives the acquisition link's MIME
  type from the stored file's own extension (`application/x-xtc` *or*
  `application/x-xtch`, not a fixed string). `OpdsParser.cpp`'s
  `isXtcAcquisition` check and `OpdsBookBrowserActivity::downloadBook()` both
  now accept and mirror back either spelling (previously only matched
  `application/x-xtc` exactly, which would have silently stopped
  `.xtch`-served books from being recognized as downloadable at all).
- Documented at `docs/opds-server-reference.md` lines 145-163 ("Acquisition
  (download) link ... **Both must be recognized as acquisition formats**").

**How this interacts with Core**: it doesn't, currently, because there's
nothing to interact with. Core's Round 8 OPDS surface (`routes.ts` line ~82,
`EPUB_MIME = "application/epub+zip"`) hard-codes every acquisition link to
`application/epub+zip` -- its own docblock states *"Core's ingestion pipeline
(Round 3 EpubValidationWorkflow) only ever produces validated EPUB masters --
there is no PDF/XTC conversion pipeline in Core at all yet."* So the
XTC-vs-XTCH parsing fix above remains correct and necessary for the legacy
server, but Core will never exercise either branch of it: **an XTC-only book
(Arabic/PDF-sourced, per the legacy "XTC-or-nothing" rule) simply has no Core
equivalent yet** -- a content-coverage gap on Core's ingestion side, not a
firmware MIME-parsing bug, and not something fixable from this repo.

## 3. TLS readiness for X3/X4 against `api.midad.one`

X3 and X4 both build from `env:default` in `platformio.ini` (single binary,
`-DFREEINK_DEVICE_X4=1 -DFREEINK_DEVICE_X3=1`, ESP32-C3/RISC-V), which extends
`[base]`. `[base]` sets `-DFREEINK_NET_WOLFSSL=1`, so both devices use the
wolfSSL-backed `SecureClient`/`SecureHttpClient` path, not plain ESP-IDF/
mbedTLS, for most HTTP(S) traffic.

**Two different TLS code paths exist today, with two different validation
postures -- this matters a lot for what "pointing at `https://api.midad.one`"
would actually mean:**

- **OPDS feed fetch, cover fetch, book download** (`HttpDownloader::fetchUrl`/
  `downloadToFile` -> `runGetSecure()` -> `runGetWolf()` on X3/X4, since
  `FREEINK_NET_WOLFSSL` is defined): calls `http.setInsecure()`
  **unconditionally** (`HttpDownloader.cpp:178`). This is the exact path the
  built-in Midad OPDS entry uses. Today it connects over plain `http://`
  anyway (`FOULAD_EBOOKS_URL = "http://midad.one/opds"`), so this is dormant,
  but if this path's URL were switched to `https://api.midad.one/opds`, the
  connection would be TLS-encrypted but **not certificate-validated at all**
  -- no chain check, no hostname check, nothing. This is not a new gap I'm
  introducing; it's the pre-existing, explicitly-flagged state described in
  `docs/tls-trust-architecture.md` ("blanket `setInsecure()`... a known,
  unresolved exposure -- not a settled design"). I have not touched this path
  or attempted to "fix" it by weakening or bypassing anything -- doing so was
  explicitly out of scope, and the repo's own recommended fix (a curated CA
  bundle) is a separate, larger piece of work that is itself still
  research-only (see below).
- **JSON POST/DELETE calls** (`runPostFile`/`runPostJson`/`runDelete` --
  device-login, reading-position, app-devices, font-convert): these build
  their own `esp_http_client_config_t` directly with
  `config.crt_bundle_attach = esp_crt_bundle_attach` and **do not** go through
  `runGetSecure()`/wolfSSL at all, regardless of `FREEINK_NET_WOLFSSL`. This is
  ESP-IDF's own real, compressed Mozilla-derived trust bundle -- genuine chain
  validation, no `setInsecure()` anywhere in this path. If these endpoints were
  pointed at an `https://api.midad.one/...` URL, they would actually validate
  the server's certificate.

**Live chain inspection of `api.midad.one`** (`openssl s_client -connect
api.midad.one:443 -servername api.midad.one -showcerts`, done as part of this
review):

```
leaf:  CN=midad.one  (SAN: DNS:midad.one, DNS:*.midad.one -- covers api.midad.one)
       issuer: C=US, O=Google Trust Services, CN=WE1
mid:   CN=WE1
       issuer: C=US, O=Google Trust Services LLC, CN=GTS Root R4
root (as sent): CN=GTS Root R4
       issuer: C=BE, O=GlobalSign nv-sa, OU=Root CA, CN=GlobalSign Root CA
Verify return code: 0 (ok)   [against the local machine's system trust store]
TLS: negotiated TLSv1.3 / TLS_CHACHA20_POLY1305_SHA256
```

Cloudflare-fronted, Google Trust Services WE1 issuing certificate, cross-signed
up to the classic self-signed 1998 "GlobalSign Root CA" for legacy-trust-store
compatibility (GTS Root R4 is itself also self-signed and present in modern
trust stores, but the server additionally serves the cross-sign so older
clients that only have the 1998 root can still validate it).

**Assessment against each validation path:**

- **`esp_crt_bundle_attach` path (JSON endpoints)**: ESP-IDF's default bundle
  is generated from Mozilla's own root program, which has included Google
  Trust Services' GTS roots for years -- this chain almost certainly validates
  cleanly through that path already. Not empirically confirmed on hardware as
  part of this review (no device flashed), but there's no specific reason to
  expect a repeat of the historical `foulad.one`/"ISRG Root YE" incident
  described in `docs/opds-server-reference.md` -- that incident was traced (per
  `HttpDownloader.cpp`'s own inline history) to a since-removed *fallback to a
  single pinned root* when the bundle failed to resolve a chain, not to the
  bundle itself being unable to handle a mainstream CA. Worth a real
  `default_tls_measure`-style hardware check against `api.midad.one`
  specifically before relying on it, but this is the lower-risk of the two
  paths.
- **wolfSSL curated-bundle path (`TlsHeapMeasureCaBundles.h`'s 12-root
  "production-candidate" bundle)**: **not wired into any production code path**
  today -- it is referenced only by the debug-only `TlsHeapMeasureActivity`
  (`grep` confirms no other `.cpp` includes `TlsHeapMeasureCaBundles.h`), so
  this finding has no live impact yet, but it's worth recording now since it's
  exactly the kind of thing that would silently bite a future "wire the
  curated bundle into `SecureHttpClient` for real" change: **the 12-root
  bundle, as currently specified, does not obviously contain a root that
  resolves this specific chain.** It lists "GTS Root R1" (Google's *other*,
  RSA-based root, used for a different certificate family) and "GlobalSign
  Root CA - R3" (the *modern* GlobalSign root, deliberately swapped in to
  replace the classic 1998 root after `c14d28b0`'s real-hardware finding that
  `www.globalsign.com` no longer chains through the classic root) -- but this
  chain terminates at "GTS Root R4" and cross-signs against the *classic* 1998
  "GlobalSign Root CA," which is the exact root the bundle deliberately
  removed. Neither "GTS Root R1" nor "GlobalSign Root CA - R3" is the same
  trust anchor as either cert this chain actually presents. **If/when the
  curated-bundle work is ever wired into production and pointed at
  `api.midad.one`, this needs a real root added (e.g. "GTS Root R4" itself,
  self-signed, which is what most modern trust stores use for this exact
  chain) and should be verified with the existing `default_tls_measure`/
  `sticky_tls_measure` hardware harness against `api.midad.one` directly,
  the same way `c14d28b0` did against `www.globalsign.com`/`example.com`.**
  I have not made this change -- the curated-bundle subsystem is explicitly
  "research and recommendation only" per its own doc header, is unrelated to
  this prep task's scope, and adding an unverified root without hardware
  confirmation would be exactly the kind of unverified assumption that
  doc warns against.

**No TLS certificate validation was weakened, bypassed, or newly disabled by
this change.** `src/FouladCoreConfig.h` adds no code path at all (see above);
nothing in this branch touches `HttpDownloader.cpp`, `SecureClient.cpp`, or
either CA-bundle file.

## 4. Remaining firmware gaps (explicit list)

1. **No Bearer-token acquisition path.** Blocking for anything beyond
   browsing Core's public OPDS feeds. Needs a Round-9-equivalent device-auth
   bridge on Core's side plus new client-side code here (OIDC device-code-ish
   flow, or a Core-specific token-exchange endpoint) -- neither exists yet.
2. **`isFouladEbooksUrl()`/`FOULAD_EBOOKS_HOST` host-allowlist doesn't cover
   `api.midad.one`.** Needs a deliberate update (and a decision on
   subdomain-matching semantics) before `X-Device-Serial`/revocation would
   work against Core.
3. **OPDS client path (`runGetWolf`) performs no TLS certificate validation
   at all**, independent of Core -- a pre-existing, already-documented,
   already-flagged gap (`docs/tls-trust-architecture.md`), not created or
   worsened by this prep work, but real and still open. Switching the live
   feed URL to `https://api.midad.one` without addressing this would mean
   "encrypted, not authenticated" for that path specifically.
4. **wolfSSL curated CA bundle, if ever wired to production, is currently
   missing a root that resolves `api.midad.one`'s actual chain** (see §3) --
   flagged now so it isn't rediscovered the hard way later.
5. **XTC-only books have no Core equivalent** (Core is EPUB-only today) --
   not a firmware gap, but a real content-coverage gap worth knowing about
   before treating Core as a drop-in replacement for the legacy catalog.
6. **Navigation depth differs** (Core's category feeds go straight to an
   acquisition feed; legacy has an extra navigation level for some
   categories) -- cosmetic, the existing OPDS parser handles both shapes fine
   (it doesn't assume a fixed depth), but worth knowing before assuming
   feature parity.
7. **No device-registration/reading-stats/device-log equivalent on Core
   yet** (per the Round 8 commit's own docblock) -- this device's
   `FouladDeviceTracking`/reading-stats code has nothing to talk to on Core
   today beyond the OPDS surface itself.

## 5. What this branch actually changed

- Added `src/FouladCoreConfig.h`: inert constants (`MIDAD_CORE_HOST`,
  `MIDAD_CORE_OPDS_URL`) recording the intended target, `#include`d nowhere,
  zero build/behavior impact.
- Added this document.

No existing file was modified. No firmware RC was built or published. No
upstream (CrossPoint) code was touched -- both new files are entirely
Midad-specific and additive, consistent with the thin-fork rule.
