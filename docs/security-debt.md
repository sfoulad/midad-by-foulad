# Security debt — open items

Tracked deliberately, so an item that was *decided* is not mistaken for an item
that was *fixed*. Each entry states what is exposed, why it is still open, and
what would close it.

## OPEN — third-party KOSync transport is unverified, and plaintext by default

**Status:** unresolved. Explicitly **not** closed by the Midad-origin fix
(commit `39941501`), which only stopped KOSync requests to Midad hosts.

**Exposure.** `lib/KOReaderSync/KOReaderSyncClient.cpp` calls
`http.setInsecure()` at all four network entry points and then sends
`KOREADER_STORE.getUsername()` together with `getMd5Password()` — an unsalted MD5
which *is* the credential in the KOSync protocol, not a hash protecting one. With
verification disabled, any machine on the path can read or alter that exchange.

Worse than unverified TLS: `KOReaderCredentialStore::getBaseUrl()` prepends
**`http://`** when the configured server has no scheme, so a user who types a bare
hostname gets the credential sent in cleartext with no TLS at all.

**Who is affected.** Any user whose sync server is not a Midad host — including
the shipped default, `https://sync.crosspointreader.com`, and self-hosted
kosync instances. Midad hosts are refused outright and are not in scope here.

**Why it is still open.** Enforcing certificate verification is a product
decision, not a mechanical change: self-hosted kosync servers commonly present
self-signed certificates, and turning verification on unconditionally would break
those users' sync with no migration path. Choosing a trust policy for arbitrary
user-supplied hosts (bundle a public-CA set? trust-on-first-use? per-server
opt-in?) is the actual open question.

**What would close it.** A decision on that policy, then: verify by default
against a public-CA anchor set, stop silently downgrading a scheme-less host to
`http://` (refuse, or require an explicit opt-in per server), and surface the
transport state in the sync UI so an insecure configuration is visible rather
than implicit.

**Do not close this by pinning.** Measured 2026-09-22: `api.midad.one` chains to
GTS Root R4 while `midad.one` chains to ISRG Root X1/X2. A single pinned anchor
does not cover Midad's own hosts, let alone arbitrary third-party ones.

## Context: what the Midad-origin fix did and did not do

Closed: KOSync requests to `midad.one`, `api.midad.one` and `foulad.one` are
refused before any credential is assembled or socket opened
(`MIDAD_ORIGIN_UNSUPPORTED`), because Core serves account-scoped resume sync at
`/resume/v1/items/:libraryItemId/position` and does not implement the KOSync
protocol on any path. Stored configuration is preserved — the device reports the
feature unavailable rather than signing the user out.

Not closed: everything in the open item above.
