# TLS Trust Architecture: OPDS / KOReaderSync / Calibre-Web-Automated

Status: **research and recommendation only. Nothing in this document is
implemented.** Covers the milestone after OTA signing in Phase 1's approved
sequence (`phase-1-plan.md` §7/§13) — closing the remaining `setInsecure()`
exposure on the wolfSSL path without repeating the documented mbedTLS
heap-crash precedent. Does not touch Calibre-Web-Automated's Basic-auth
semantics anywhere in this design.

## Headline correction to the Phase 1 plan's original assumption

The original plan (and Milestone 1's TLS containment work) stated wolfSSL's
`SecureHttpClient`/`SecureClient` "only expose `setInsecure()` or
`setCACert(singleRootPem)` — no CA-bundle facility... real net-new SDK/library
work." **This is wrong, verified by reading the actual vendored wolfSSL source
this project builds against (`Arduino-wolfSSL @ 5.7.2`, matching
`platformio.ini`'s `lib_deps`), not just the FreeInk SDK wrapper's doc
comment.**

`SecureClient::setCACert(const char* rootCA)` (`SecureClient.cpp:22,85-88`)
passes whatever buffer it's given straight to wolfSSL's own
`wolfSSL_CTX_load_verify_buffer(ctx, rootCA, strlen(rootCA),
WOLFSSL_FILETYPE_PEM)`. That function's real implementation
(`ssl_load.c:3765-3781`) explicitly comments *"When PEM, treat as certificate
chain of CA certificates"* and calls `ProcessChainBuffer()`
(`ssl_load.c:2438-2489`), which is a `while` loop that keeps parsing
consecutive `-----BEGIN CERTIFICATE-----` blocks out of the same buffer until
it runs out of bytes, loading every valid one it finds into the CTX's trust
store. **A concatenated multi-certificate PEM bundle passed to the existing
`setCACert()` call already works today, at the wolfSSL layer, with zero
FreeInk SDK or Midad code changes.** The SDK doc comment ("Verify against a
single PEM root") describes the only way it's been *used* so far, not a
code-level limit.

This means the real work here is a **memory-budget and content-curation
problem** (how big a bundle can this device afford, and which roots actually
matter), not an API-design problem.

## Why the historical mbedTLS crash doesn't directly answer that question

The 57KB→5.4KB free-heap crash (`git show 59cf4c89`, `HttpDownloader.cpp`'s
own inline history) was specifically: verifying against **a single pinned
root forced mbedTLS off ESP-IDF's `esp_crt_bundle_attach` fast
lookup-and-reject path and onto a full chain-build/verify of the *server's*
4-certificate presented chain** — a fundamentally more expensive code path
than the bundle's optimized matching, not a cost that scales with "how many
roots are trusted." mbedTLS and wolfSSL are different TLS stacks with
different certificate-manager implementations, so this precedent doesn't
transfer mechanically — but the underlying lesson does: **a device with ~50KB
free heap during a reading session cannot assume real chain verification is
cheap just because it "should" be, and must have that confirmed empirically on
this exact hardware/stack before shipping**, the same way Milestone 1's OTA
spike caught a wrong assumption about PlatformIO's build pipeline by actually
running it.

**Not yet measured, flagged as required before implementation**: actual wolfSSL
heap cost of verifying a real server's chain against an N-root trust store on
this hardware. The existing KOSync heap gate (`MIN_FREE_FOR_TLS = 35000`,
`MIN_BLOCK_FOR_TLS = 20000`, `KOReaderSyncClient.cpp`) was calibrated for a
handshake that does **zero** certificate verification (`setInsecure()` skips
it entirely) — adding real verification adds cost on top of what that gate
already assumes, and the gate will likely need to be re-calibrated higher,
not just reused as-is.

## Estimated per-root memory cost (source-derived, not measured on hardware)

wolfSSL represents each loaded trusted CA as a `Signer` struct
(`wolfssl/wolfcrypt/asn.h:2007-2048`): fixed overhead (~90-130 bytes:
key metadata, name-constraint pointers, several 20-byte SHA hashes) plus a
separately-allocated public key buffer (`publicKey`, pointed to, not inline —
roughly 50-70 bytes for an EC P-256 root, ~270-420 bytes for an RSA-2048/3072
root) plus a variable-length common-name string (`name`, typically 20-60
bytes). This project's `user_settings.h` does **not** define
`WOLFSSL_SIGNER_DER_CERT`, so the full DER certificate is **not** retained per
signer (confirmed by grep) — if it were, each entry would cost roughly
800-2000 bytes more. Net estimate: **roughly 200-600 bytes of heap per
trusted root**, dominated by whether that root uses an RSA or EC key. A full
browser/Mozilla-style trust store (130+ roots) would cost on the rough order
of 40-80KB+ — implausible on this device even before adding handshake
overhead. A **curated bundle of 10-15 roots** is a more realistic ~2-9KB,
which is a plausible fit but still needs on-device confirmation, not just this
estimate.

**Architectural note on when this cost is paid**: `SecureClient::
connectWithMethod()` creates a brand-new `WOLFSSL_CTX` (and therefore
re-parses the bundle) on every fresh `connect()` — not once at boot.
`SecureHttpClient`'s connection-reuse (`setReuse(true)`, default) keeps one
CTX alive across multiple HTTP requests to the *same* host within one
`SecureHttpClient` instance's lifetime, so the cost isn't paid per-request,
but it *is* paid again each time a new logical operation starts a new
`SecureHttpClient` (a fresh OPDS browse session, a fresh KOSync call). Worth
knowing before assuming "load once, forget about it."

## Options evaluated

| Option | RAM/flash impact | Compatibility | Recommendation |
|---|---|---|---|
| **Curated CA bundle** (10-15 major public roots: ISRG Root X1/X2 for Let's Encrypt — very common on self-hosted CWA/OPDS instances — plus a handful of major commercial CAs) | Bundle text itself is flash-resident (`static constexpr` PEM string, per CLAUDE.md's Flash Persistence rule); parsed `Signer` cost ~2-9KB heap per active session, not yet hardware-confirmed | Covers `midad.one`/crosspoint-sync and most public OPDS catalogs (which almost universally use a small set of major CAs) | **Primary mechanism for public/known servers.** Needs on-device heap measurement before shipping — do not assume the estimate holds |
| **Trust-on-first-use (TOFU) + SPKI pinning** for self-signed/local servers | Trivial (~32-64 bytes per pinned host: a SHA-256 of the leaf's public key, stored in `PersistableStore`/settings, not a full cert) | The only realistic option for self-signed Calibre-Web-Automated instances and local-network OPDS servers, which will never chain to any public root | **Primary mechanism for self-signed/local.** Requires new UI: a "this server's certificate isn't from a known authority — trust it?" prompt, replacing today's silent blanket `setInsecure()`. No existing code does this today (confirmed: no `setCACert`/trust-prompt UI exists anywhere in `src/activities/`) — this is genuinely new surface, not an extension |
| **User-supplied custom CA** (paste/import a root PEM for a specific server) | One PEM string in settings, same per-root cost as the curated bundle | Covers advanced self-hosters running their own internal CA | Optional, secondary to TOFU pinning — TOFU covers the common case with less user friction; expose custom-CA import only if TOFU proves insufficient for a real reported case |
| **Certificate/public-key pinning generally** | Same as TOFU above | N/A — this *is* the mechanism TOFU uses | Recommended specifically for the self-signed case, not as a blanket replacement for CA-based trust on public servers (pinning a public CA-issued cert's exact key would break the moment that server rotates certs, which Let's Encrypt does every ~90 days) |
| **HTTP (no TLS) for local servers** | No change | Already the existing model per Milestone 1's TLS containment findings | Keep as-is: allowed only via explicit user action, never silently auto-selected or silently downgraded from https |
| **TLS 1.2 vs 1.3** | No change | Already both supported (`wolfSSLv23_client_method` auto-negotiates, explicit `wolfTLSv1_2_client_method` fallback retry) | No design change needed — certificate verification happens above the TLS version-negotiation layer, identically regardless of which version was negotiated |
| **mbedTLS instead of wolfSSL for this path** | wolfSSL's SP_ECC-optimized handshake crypto is already measured cheaper (~30-40KB transient) than mbedTLS's historical ~48KB+ peak (per `KOReaderSyncClient.cpp`'s existing heap-gate comments) | mbedTLS still can't do TLS 1.3 in this build (the original reason wolfSSL was adopted) | Not recommended — reverting to mbedTLS for verified traffic would reopen the TLS-1.3 problem wolfSSL was adopted to fix, for no proven memory benefit on the *verification* cost specifically (unmeasured for both) |
| **Full Mozilla/browser-grade trust store** | ~40-80KB+ estimated | Best real-world compatibility | Not recommended — implausible on a ~50KB-free-heap device even before handshake overhead; the curated-bundle + TOFU combination covers the realistic server population this device actually talks to |

## Recommended architecture

1. **Curated CA bundle** (flash-resident PEM, wired through the *already-existing*
   `setCACert()`) becomes the default trust check for OPDS and KOSync/CWA
   connections, replacing the blanket `setInsecure()`.
2. When a handshake's certificate verification fails against that bundle
   (a self-signed server, or a public server on a CA the bundle doesn't
   include), fall through to a **TOFU prompt**: show the user the server's
   certificate fingerprint, let them explicitly accept it, then pin its SPKI
   hash for that host going forward. Never silently fall back to
   `setInsecure()` in that case — that's today's blanket-unverified behavior,
   the exact thing this milestone exists to close.
3. Re-verify against the pinned SPKI on every subsequent connection to that
   host; if the server's key ever changes unexpectedly, treat it the same as
   a first-time untrusted server (re-prompt), not a silent pass-through.
4. Calibre-Web-Automated's Basic-auth semantics are completely unaffected by
   any of this — this design only changes *transport* trust (is this
   connection to *this* server verified), never *authentication* (the
   username/password sent once the connection exists). No code in
   `applyAuthHeaders()` (`KOReaderSyncClient.cpp`) needs to change.
5. **Do not implement any of this until the memory estimate above is
   confirmed on real hardware** — build a throwaway test harness that loads
   the intended curated bundle and performs a real handshake against a
   representative server (e.g. `midad.one` and a self-hosted CWA-style
   self-signed endpoint), measuring free heap before/during/after, on a
   dedicated test device (same hardware-gate discipline as OTA Part E — the
   reference device stays off-limits for this too).

## User configuration implications

- **No new setting needed for the common case** — a public OPDS/KOSync server
  on a bundle-covered CA just works, same as today's "it connects" experience,
  minus the silent lack of verification.
- **New UI required**: a certificate-trust prompt (shown once per
  new/changed self-signed server) and a way to review/revoke previously
  trusted self-signed servers from Settings — this is genuinely new surface,
  not a copy of an existing pattern in this codebase.
- **No change to CWA login UX** — username/password entry stays exactly as it
  is; only what happens at the transport layer underneath it changes.

## Explicitly not decided by this document
- The exact curated root list (needs a real survey of what OPDS/KOSync/CWA
  hosts in the field actually present — starting candidates: ISRG Root X1/X2,
  DigiCert Global Root, Sectigo/USERTrust, GlobalSign Root — not finalized
  here).
- Exact heap-gate threshold numbers for the new, heavier handshake (needs
  hardware measurement, see above).
- Where pinned-server state lives (`PersistableStore` is the obvious existing
  mechanism per `CLAUDE.md`'s architecture, but the exact schema isn't
  designed here).

**This document is a recommendation for review, not an implementation
plan-of-record. Stopping here per instruction.**
