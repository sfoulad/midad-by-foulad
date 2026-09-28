# Thin-fork baseline record — X3/X4 Pro convergence, September 2026

Records the Midad-side divergence the owner approved when the X3 consolidated
line and the X4 Pro convergence line were merged into one shared codebase.

**This is a baseline record, not an exemption.** It does not change
`scripts/thin-fork-guard.sh`, does not add a path allowlist, and does not
authorise any future divergence. It states what was reviewed and accepted, so
the guard's verdict can be read against a recorded decision instead of being
re-derived by hand each time.

## Scope

    integration branch  integration/1.6.22-x4pro-touch
    upstream baseline   7db14a015ca4b1d74d3cbed131a748f3029a4517
                        (CrossPoint 1.6.0rc-80-g7db14a01, 2026-09-06)
    guard base used     72b153fd (the 1.6.22 line, this merge's first parent)

## Correction to the file count

The guard lists **54** files. Ten of those are **not divergences**: their content
at HEAD is byte-identical to upstream at `7db14a01`. The merge moved them
*toward* upstream, and the guard reports them only because they differ from the
base ref, without checking whether the new content equals upstream's.

    lib/EpdFont/EpdFontData.h
    src/activities/settings/OpdsServerListActivity.h
    src/activities/settings/TextSettingsActivity.h
    src/activities/settings/TextSettingsPreview.cpp
    src/activities/settings/TextSettingsPreview.h
    src/util/DictHtmlPages.h
    src/util/DictionaryRegistry.cpp
    src/util/DictionaryRegistry.h
    src/util/HtmlToPlainText.cpp
    src/util/HtmlToPlainText.h

**The approved divergence set is therefore 44 files, not 54.** Of those 44, all
were already divergent from `7db14a01` on the X4 Pro convergence line before
this merge — no file in the set is newly hand-authored divergence.

## The 44, by why the difference is necessary

- **TOUCH-UI** — upstream ships no touch Settings, preview or option popup.
  These are the single shared architecture that lets one build serve X3's
  buttons and X4 Pro's touch panel. Explicitly *not* a second architecture.
- **ARABIC/RTL** — kashida justification, RTL shaping, Arabic-aware
  HTML-to-text and the dictionary above them. Upstream has no Arabic support.
- **PLATFORM** — board/capability plumbing on CrossPoint's own
  `FREEINK_CAP_*` and `BoardConfig::` abstractions; no Midad board switch.
- **CORE+OTA** — the Midad Core client (`api.midad.one`, pinned at GTS Root
  R4) and the digest-authenticated OTA path. Upstream OTA is unpinned.
- **HARDENING** — Wi-Fi connect-policy state machine and CSS cache-version
  reconciliation, both answering field-observed failures.
- **READER** — toolbar/overlay, long-press page skip, dictionary entry,
  KOReader sync.
- **FONTS** — SD font catalogue and the split kerning-class record layout.

Per-file assignment: `midad-verification-package/reports/divergence-classification.md`.

## What this record does NOT do

The guard still reports FAIL, and that is correct. Two things gate it, and
both need a repo admin — neither can be done from a working tree:

1. **The baseline has to actually move.** The guard measures against the PR's
   base branch. Against `origin/develop` this branch shows 222 divergent files,
   because develop carries neither the 1.6.22 sync nor either RC line. The
   divergence becomes "pre-existing" only once this work is on develop.
2. **`.github/workflows/ci.yml` is a security-boundary file** and differs from
   develop. It is NOT touched by this integration — it is identical to the
   1.6.22 line — but the boundary gate is path-based, not authorship-based, so
   it fires regardless. Clearing it requires the documented procedure in
   `docs/upstream-sync-architecture.md` ("Security-boundary file maintenance"):
   admin relaxes the required check, merges, restores immediately.

Recording the decision does not substitute for either step.

## Addendum — CrossPoint 1.6.5rc merge

    upstream merged     a1ceb6333f907856e4dcde21bd0a94f5be84452b (1.6.5rc)
    merge base          7db14a015ca4b1d74d3cbed131a748f3029a4517

Divergences **deliberately kept** in this merge. Each was reached by reading both
sides, not by taking a whole file from one of them; a future sync should not
re-open them without a product decision.

| Area | Kept | Why |
| --- | --- | --- |
| `RecentBooksActivity.{h,cpp}` | ours (upstream deleted it) | Midad's Home hero/recents row. Five dependents: `HomeActivity`, `ActivityManager`, `GridNav`, `QuranBook`, `GymCatalog`. |
| `OpdsBookBrowserActivity.cpp` | ours | Midad renders the catalogue as a direct cover grid via `render(RenderLock&&)`; upstream moved to declarative `UiScreen` builders this fork's header does not declare. |
| `HomeActivity` tile list | ours | Midad's Home is four tiles (Foulad eBooks, Stats, Apps, Settings). `menuItemToIndex`/`indexToMenuItem` must stay mutual inverses of that list. |
| `SettingInfo` location | ours (`SettingsTypes.h`) | This fork extracted it; upstream still defines it inside `SettingsActivity.h`. Taking upstream's copy would be a redefinition. Upstream's two new `SettingAction` values were ported across. |
| `uiTheme` settings row | absent | Single-theme firmware (`uiTheme = FOULAD`). |
| `main.cpp` idle loop | ours | BLE-aware light sleep; upstream's simpler loop cannot express the BLE RF-floor guard, and the code after it reads `blePreventsLowPower`/`idleMs`. |
| `test/minibidi_arabic` | not carried | Asserts `BidiUtils` emits Arabic presentation forms. This fork shapes at draw time in `lib/ArabicShaper`; `test/arabic_shaper` covers the same forms in the right architecture. |
| `PngToBmpConverter` internal | both parameters | Both sides added a 7th positional `bool`. `allowUpscale` keeps position 7 so existing callers bind unchanged; upstream's `originalThresholds` is appended at 8. |

Defects this merge found and fixed, rather than carried forward:

- `EpdFont::getLigature` was missing upstream's #3294 guard, so the font's
  Lam-Alef pairs re-collapsed already-shaped Arabic presentation forms and
  transposed letters. `test/ligature_guard` existed but was never registered.
- `SdCardFont`'s underuse hysteresis ended up evaluated **twice** (both sides had
  moved it, to different functions), retiring font buffers a scope early.
  Upstream's cache-miss placement is the one kept.
- `SdCardFont`'s arena retry (`PREWARM_ARENA_TOO_LARGE`, `measuredBytesPerGlyph`)
  had been dropped by the earlier 1.6.22 merge (`1a8fb18c`) and is restored.
- `KeyboardLayoutsActivity` compiled but was unreachable: its settings row and
  dispatch case were lost.
- OTA asset naming: the release workflows publish `crosspoint-<version>-<device>.bin`
  while the firmware still searched `firmware.bin`. All three publishing workflows
  and `OtaUpdater` now agree, including the leading-`v` strip Midad's tags need.

## Security-boundary verification — `.github/workflows/ci.yml` (1.6.5rc merge)

The 1.6.5rc merge carries one upstream change into a `SECURITY_BOUNDARY_FILES`
path, so `scripts/thin-fork-guard.sh` fails this PR by design. The guard was
**not** weakened, bypassed, or given an allowlist entry. What the delta is,
verified structurally rather than by eye:

    ours     0d5a9fc7
    merged   a940f489
    delta    .github/workflows/ci.yml, +4 / -3

1. A comment correction. The old text claimed PR artifact names match the
   release asset names OtaUpdater requests — no longer true now that releases
   publish `crosspoint-<version>-<device>.bin`. The new text says the artifact
   names are for PR identification only.
2. One matrix entry added to the `build` job: `{env: x4c, asset: firmware-x4c.bin}`.

Parsed both revisions and compared structurally:

| Property | Result |
| --- | --- |
| `on:` (triggers) | identical |
| top-level `permissions:` | identical |
| job list | identical (7 jobs) |
| every job's `steps`, `if`, `needs`, `permissions` | identical |
| whole file with the `build` matrix excluded | **byte-identical** |
| `test-status` (the required check) | untouched |

No trigger, permission, gate condition, required-check name, or enforcement path
changed. The one behavioural effect is **stricter**, not weaker: `build` now has
to compile a fifth board (`x4c`) before it can go green.

Procedure status: this is `docs/upstream-sync-architecture.md`'s
"Security-boundary file maintenance" case. Step 1 (classify) and the verification
above are done. **Steps 2-4 — the admin temporarily relaxing
`thin-fork-guard-trusted` in the ruleset, merging, and immediately restoring it —
remain outstanding and are the repo owner's to perform.** Step 5 does not apply:
`ci.yml` is the CI entrypoint, not the trusted evaluator's own logic, so no
disposable-PR re-validation of the guard is required.

Until then the guard stays **RED**, which is the correct state.

## Temporary SDK fork dependency (tracks Free-Ink/freeink-sdk PR #65)

`.gitmodules` points `freeink-sdk` at **`https://github.com/sfoulad/freeink-sdk.git`**
instead of `Free-Ink/freeink-sdk`. This is a fetchability workaround, not a
divergence in SDK content, and it is **temporary**.

    pinned gitlink   c4906def5a3f46456b743d55defc87de054fa226
                   = e30d25a0 (upstream 1.6.5rc's own pin) + exactly 1 commit
    fork branch      pin/midad-1.6.5rc-secureclient-peer-verification
    upstream PR      Free-Ink/freeink-sdk #65 (open)

**Why it was necessary.** The pin had only ever existed in a local submodule
checkout, so every CI job — builds, cppcheck, CodeQL, clang-format, unit tests
and the thin-fork guard alike — died at `actions/checkout` with
`upload-pack: not our ref`. The branch had never had a green CI run; the failure
predates this merge (head `0d5a9fc7`, 2026-09-16, failed identically on the older
pin `e693f2bc`). A green local four-board build does not detect this, because the
submodule is already present on disk.

**Why not just pin upstream's `e30d25a0`.** The extra commit is not "the
hostname-verification fix" alone -- it makes three changes:

1. **Hostname verification.** Arms `wolfSSL_check_domain_name()` on every
   non-insecure connection. Without it a `setCACert()` connection accepts any
   certificate signed by any trusted CA **for any hostname**, so a
   CA-verified connection is still redirectable to any CA-certified attacker
   host. SNI names the host but does not bind the peer certificate to it.
2. **Fail-closed CA loading.** `wolfSSL_CTX_load_verify_buffer()` failure now
   aborts the connect instead of proceeding to a handshake against an empty
   trust store, whose verify error is indistinguishable from a genuine bad peer.
3. **RFC 9110 s6.4.1 no-body statuses.** `SecureHttpClient` stops reading a body
   for 1xx/204/304 regardless of the `Content-Length`/`Transfer-Encoding` those
   responses echo.

Dropping the pin would be a real TLS regression on (1) and (2).

**Coverage of (3), stated honestly: it is latent in this firmware today.**
`runGetWolf` is the only `SecureHttpClient`-backed path, and its only caller is
`OtaUpdater`'s pinned GitHub release-metadata fetch, which deliberately sends no
`If-None-Match` (the conditional/ETag path is abandoned on that transport --
see the comment at `OtaUpdater.cpp`). The one firmware path that does expect a
204, `runDelete`, runs on `esp_http_client`, not the SDK client. So no current
request can receive a no-body status over the changed code. It is carried
because it removes a real stall hazard -- such a response with no framing
headers would otherwise fall into `readUntilClose()` and hang a kept-alive
connection until the timeout -- and because it is a prerequisite for ever
re-enabling conditional GETs on the pinned transport. There is no host test for
it: the SDK ships no test suite, and the behaviour is not reachable from this
firmware's own paths.

**Why not pin PR #65's own branch.** Its head (`6d33e9fd`) is based on an SDK
older than `e30d25a0`, so pinning it would regress the SDK base and contradict
the directive to adopt upstream's 1.6.5rc pin.

**Exit condition.** Revert the URL to `https://github.com/Free-Ink/freeink-sdk.git`
as soon as equivalent hostname verification is merged upstream onto a base at or
after `e30d25a0`, and re-pin to that upstream commit. Until then this fork must
carry **only** this one commit — it is not a place to accumulate SDK divergence.
