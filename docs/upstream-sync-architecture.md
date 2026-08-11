# Midad as a Thin Fork of CrossPoint: Architecture and Migration Plan

## Status: Phase A scaffolding complete; conflict path validated. Phase B (settings extraction) complete across three review rounds, pending merge (PR #136) -- see "Phase B round 3" below for the final state. Clean-merge CI/PR path still remains to be exercised once a clean merge is achievable. Phases C onward not started.

## Phase B final state (after round 3)

Read this section first if you only want the current facts, not the history
of how Phase B got there (that's in the round 1/2/3 sections below, kept for
the record).

- **`MidadAppSettings`** (`src/MidadAppSettings.h/.cpp`) owns **13 fields**:
  `quranEnabled`, `rssEnabled`, `gamesEnabled`, `tasbihEnabled`,
  `stopwatchEnabled`, `pomodoroEnabled`, `pomodoroFocusMin`,
  `pomodoroShortBreakMin`, `pomodoroLongBreakMin`, `gymEnabled`,
  `gymWeightUnit`, `debugLoggingEnabled`, `bleEnabled`. None have a
  CrossPoint upstream equivalent.
- **`SettingsList.h`** (upstream-owned) contains **zero** Midad-specific
  executable lines — no `MIDAD_APP_SETTINGS`/`MidadAppSettings` reference of
  any kind, only two comments pointing at `src/MidadSettingsList.h/.cpp`.
- **`CrossPointSettings.h`** no longer declares `bleEnabled` (or any of the
  other 12) — confirmed absent.
- **`/api/settings` GET/POST** (`CrossPointWebServer.cpp`) exposes and
  accepts all 13 Midad fields via `getCombinedSettingsList()`
  (`src/MidadSettingsList.h/.cpp`), which merges `getSettingsList()` with
  `appendMidadAppSettings()` — the single source of truth both the web API
  and (indirectly, via its own two-call composition) the on-device Settings
  screen build from.
- **`FouladDeviceTracking`'s settings.push/report wire contract**: unchanged
  from before Phase B for the 8 fields it already carried (quran/rss/games/
  tasbih/stopwatch/gym/gymWeightUnit/debugLogging — now read from
  `MidadAppSettings` instead of `CrossPointSettings`, same JSON keys); the 3
  pomodoro fields and `bleEnabled` were never part of that contract and still
  aren't — no new remote wire behavior added anywhere in Phase B.
- **Fixed-upstream conflict count**: **158**, unchanged across all three
  rounds (down from JsonSettingsIO's 2 modify/delete conflicts, up by the
  `BookmarkFile` add/add pair and one `PersistableStore.h` visibility
  conflict — see "Phase B results" below for the original accounting; rounds
  2 and 3 introduced zero additional conflicts because every file they
  touched was already on the list, and every new file they added is
  Midad-only and can't conflict).
- **Tests**: 116 gtest cases pass (112 pre-existing + 4 new
  `MidadAppSettingsLoadPlan` cases added in round 2; round 3 added no new
  test target). `pio run -e default`/`simulator` succeed; `pio check`
  (cppcheck) reports no defects; `clang-format-fix` is clean.

## Phase B round 3: web API regression fix, bleEnabled extraction

Follow-up to round 2, per PR #136 review feedback:

1. **Restored Midad settings to `/api/settings`.** Round 2 moved the Midad
   row definitions out of `SettingsList.h` correctly for the on-device
   Settings screen (`SettingsActivity::rebuildSettingsLists()` calls
   `appendMidadAppSettings()`), but `CrossPointWebServer::handleGetSettings()`/
   `handlePostSettings()` still called `getSettingsList()` directly — which no
   longer includes the Midad rows. This was a real regression: the 13 Midad
   fields silently disappeared from the web settings API after round 2.
   Fixed with `getCombinedSettingsList()` (`src/MidadSettingsList.h/.cpp`), a
   Midad-owned helper that returns `getSettingsList()` with
   `appendMidadAppSettings()` merged in; both `CrossPointWebServer` handlers
   now call it. `SettingsActivity` is unaffected — it still needs its own
   category-then-action ordering (Dictionary/KOReader Sync between the
   generic list and the Midad rows), so it keeps its round-2 two-call
   composition rather than switching to the combined helper.

2. **Finished the `bleEnabled` extraction.** It had no CrossPoint upstream
   equivalent either, so leaving it on `CrossPointSettings` (and its row in
   `SettingsList.h`) was the same permanent fork delta as the other 12
   fields, "live radio switch" notwithstanding. Moved it to `MidadAppSettings`
   as the 13th field: `toJson`/`fromJson` (the existing legacy migration
   covers it for free, since `fromJson()` is reused for both normal loads and
   migration), `MidadSettingsList.cpp`'s row definition (removed from
   `SettingsList.h`, which now has zero Midad-specific rows left),
   `AppsActivity`'s live BLE tile toggle/label, and `main.cpp`'s Idle-state
   BLE gate. `bleEnabled` was never part of `FouladDeviceTracking`'s
   settings.push/report wire contract before this move (confirmed via
   `grep`) and still isn't — no new remote wire behavior added.

Verified live in the simulator (temporary hook replicating
`handleGetSettings()`/`handlePostSettings()`'s exact switch-case logic against
`getCombinedSettingsList()`, reverted before committing — `git status`
confirmed clean both times, including a caught-and-fixed mistake where the
first `git checkout --` of `main.cpp` reverted the legitimate `bleEnabled`
gate fix along with the temporary hook, since both were uncommitted changes
to the same file): GET returned `quranEnabled`, `pomodoroFocusMin`,
`gymWeightUnit`, `debugLoggingEnabled`, and `bleEnabled` with correct
defaults (`0`, `25`, `0`, `0`, `1`); POST applied all 5
(`applied=5`) and persisted them to `midad_apps.json`, including
`bleEnabled` flipping `1` → `0`.

**Conflict re-measurement**: **158, identical file list to rounds 1 and 2.**
`CrossPointSettings.h`, `CrossPointWebServer.cpp`, and `main.cpp` were all
already conflicting; `AppsActivity.cpp` isn't an upstream file at all and was
never on the list. Zero new conflicts from this round either.

## Phase B round 2: SettingsList.h cleanup, hardened migration, wire-path test

Follow-up to the round-1 results below, per PR #136 review feedback:

1. **Finished the SettingsList extraction.** Round 1 moved the 12 fields off
   `CrossPointSettings` but left their `SettingInfo::DynamicToggle`/
   `DynamicValue`/`DynamicEnum` row *definitions* (the actual lambda code
   referencing `MIDAD_APP_SETTINGS`) inside `SettingsList.h` — still
   Midad-owned behavior sitting in an upstream-owned file. Moved all of it
   (Quran/Games/Tasbih/Stopwatch/Pomodoro+3/Gym/GymWeightUnit/Debug Logging)
   to new `src/MidadSettingsList.h/.cpp`, behind one hook:
   `appendMidadAppSettings(appsSettings)`, called once from
   `SettingsActivity::rebuildSettingsLists()`. `SettingsList.h` now contains
   **zero** `MIDAD_APP_SETTINGS`/`MidadAppSettings` references — only two
   comment lines pointing at where the code lives. The generic
   `SettingInfo::DynamicToggle`/`DynamicValue` factories stay in
   `SettingsActivity.h` (generic infrastructure, not Midad product logic, per
   the review's own carve-out), and `CrossPointWebServer.cpp`'s matching
   `valueGetter`/`valueSetter` fallback stays too, flagged as a possible
   future upstream-contribution candidate. One accepted side effect: Quran
   through GymWeightUnit now render after Dictionary/KOReader Sync in the
   on-device Apps list instead of before (Debug Logging's "always last"
   position is unchanged) — a cosmetic reorder, not a behavior change.

2. **Hardened the migration.** `MidadAppSettings::loadFromFile()` previously
   treated *any* load failure — file missing, or file present but
   corrupt/unparseable — as "attempt migration from legacy settings.json."
   That's wrong: a corrupt `midad_apps.json` should report failure, not
   silently resurrect stale pre-migration values. Extracted the decision
   into a pure function (`src/MidadAppSettingsLoadPlan.h/.cpp`,
   `planMidadAppSettingsLoad(ownFileExists, ownFileLoadedOk,
   legacyFileExists)`) with zero HalStorage/ArduinoJson dependency, so it's
   host-testable without any HAL mocking — a new gtest target,
   `test/midad_app_settings_load_plan/`, covers all four outcomes
   (`UseOwnFile`, `ReportCorrupt`, `Migrate`, `FreshDefaults`). Also
   live-verified in the simulator: a hand-corrupted `midad_apps.json` (`"not
   valid json {{{"`) alongside a legacy `settings.json` correctly logged
   `[ERR] [MAS] midad_apps.json exists but failed to load; not migrating
   from legacy settings.json` and left the corrupt file untouched — no
   silent fallback.

3. **Exercised the wire path directly**, not just inspected key names. Added
   a temporary simulator-only hook (`FouladDeviceTracking::
   debugTestApplySettings()`, called from `main.cpp` behind `getenv(
   "SIM_TEST_SETTINGS_PUSH")`) that pushes a payload touching a toggle
   (`quranEnabled`), an enum (`gymWeightUnit`), and one unrelated
   `CrossPointSettings` field (`darkModeEnabled`) through
   `applySettingsFromServer()`, then calls `addSettingsReport()` and writes
   the result to a file for inspection. Confirmed: `MidadAppSettings` values
   changed correctly (`quranEnabled` 0→1, `gymWeightUnit` 0→1), `midad_apps.
   json` was persisted with those values, fields *not* in the payload stayed
   at their compile-time defaults (no cross-talk), `settings.json` picked up
   `darkModeEnabled` correctly with zero Apps-category keys left in it
   (confirming the split is clean), and `addSettingsReport()` read every
   pushed value back under its original wire key. The temporary hook was
   fully reverted (`git checkout --`) before this commit — `git status`
   confirmed clean. Pomodoro fields remain deliberately unwired (matching the
   pre-existing gap noted in round 1), per the explicit instruction not to
   expand wire behavior in this PR.

**Conflict re-measurement** (same method: real `git merge upstream/develop`
against `crosspoint-reader/develop @ 2cac5971`, same commit every measurement
in this doc has used): **158 conflicting files, identical file list to round
1.** Every file this round's changes touched (`SettingsActivity.h/.cpp`,
`SettingsList.h`, `test/CMakeLists.txt`) was already conflicting before this
round — shrinking Midad's footprint inside an already-conflicting file
doesn't change the file-level count, only what has to be reconciled inside
it. The four new files this round added (`MidadSettingsList.h/.cpp`,
`MidadAppSettingsLoadPlan.h/.cpp`, plus the new test directory) don't exist
upstream at all, so none of them could conflict. Net: zero new conflicts
introduced by a real architectural cleanup — the best possible outcome for
this kind of change.

## Phase B results: MidadAppSettings extraction + BookmarkFile adoption

Built `MidadAppSettings` (`src/MidadAppSettings.h/.cpp`), a `PersistableStore<T>`
singleton holding the 12 fields identified in the settings-migration inventory
above (`quranEnabled`, `rssEnabled`, `gamesEnabled`, `tasbihEnabled`,
`stopwatchEnabled`, `pomodoroEnabled`/`FocusMin`/`ShortBreakMin`/`LongBreakMin`,
`gymEnabled`/`gymWeightUnit`, `debugLoggingEnabled`) — every one of them had zero
CrossPoint upstream equivalent, so every touch to `CrossPointSettings.h`/
`SettingsList.h` for them was permanent, avoidable fork delta. Two new
`SettingInfo::DynamicToggle`/`DynamicValue` factories (mirroring the existing
`DynamicEnum`/`DynamicString` pattern `KOReaderCredentialStore` already used)
made this possible without inventing a new settings-UI mechanism.
`AppsActivity::AppEntry::enableFlag` (a `CrossPointSettings`-typed
pointer-to-member) became getter/setter `std::function`s for the same reason.
`FouladDeviceTracking.cpp`'s wire keys are unchanged — only the backing store
moved, verified field-by-field against the existing key list.

**Migration**: `MidadAppSettings::loadFromFile()` seeds itself from the legacy
keys still sitting in `settings.json` the first time its own file doesn't
exist yet (same technique `CrossPointSettings` already uses for its
`settings.bin` → `settings.json` migration). Verified live in the simulator:
booted with a synthetic pre-Phase-B `settings.json` (all 12 fields set to
non-default values, no `midad_apps.json` present) and confirmed the migration
log fires exactly once, all 12 values carry over exactly, and a
migration-dependent behavior (Quran extraction, gated on the migrated
`quranEnabled`) actually ran. A second boot confirmed no re-migration.

**Also closed in the same PR**: adopted upstream's own `BookmarkFile` module
(commit `63eda54e`, the same "migrate off `JsonSettingsIO`" commit discussed in
the Phase A section above) for the two remaining `JsonSettingsIO` call sites
(bookmark load/save) instead of keeping a permanently-diverged remnant of that
file. `JsonSettingsIO.cpp/.h` deleted outright. This required making
`PersistableStoreBase::read/writeDocToFile` public — upstream had already made
the same change, for the same reason (a free function, not a `PersistableStore`
subclass, needs to call them) — so this is a second, independent point of
convergence with upstream's shape, not just the bookmark logic itself.

**Real merge-conflict delta** (measured the same way as Phase A: a real
`git merge upstream/develop --no-ff` against `crosspoint-reader/develop @
2cac5971` — the identical upstream commit Phase A measured against, so this is
an apples-to-apples comparison with no upstream drift in between — this time
off `feat/phase-b-settings-extraction` instead of `develop`, since the PR
hasn't merged yet):

- **Old baseline (Phase A): 157 distinct conflicting files** (158 conflict
  events — `portuguese.yaml`'s rename/delete and `portuguese-BR.yaml`'s
  modify/delete both landed on the same path).
- **New count (Phase B): 158 distinct conflicting files.**
- **Eliminated**: `src/JsonSettingsIO.cpp`, `src/JsonSettingsIO.h` (2 files) —
  gone because we deleted them, matching upstream having already deleted them.
- **Introduced**: `lib/Serialization/PersistableStore.h` (a real, new content
  conflict — see below), `src/util/BookmarkFile.cpp`, `src/util/BookmarkFile.h`
  (2 files, both **add/add** — both sides independently created the same file
  at the same path, because we deliberately copied upstream's own version).

**The raw count is flat, but that number hides the real result.** An add/add
conflict on two files that are already near-identical (ours is upstream's
`BookmarkFile.cpp`/`.h` verbatim, minus the `visibleTextOffset`/`vo` field
upstream's version also carries — a genuinely separate, out-of-scope feature)
is close to a mechanical resolution — likely "take upstream's version, or
diff the two and confirm they agree" — nothing like the judgment call the old
`JsonSettingsIO` modify/delete conflict demanded (decide whether to accept
upstream's deletion at all, then work out where the bookmark logic that stayed
should live). `PersistableStore.h`'s new conflict is a single, deliberate,
2-line visibility change (`protected:` → `public:` for one pair of methods)
landing in a region upstream has also independently restructured (upstream's
own `PersistableStoreBase` grew a built-in `storeMutex`/`resaveRequested`
mechanism this fork's version doesn't have — a real, larger design divergence
worth its own future phase, not something this change tried to fix).

**Where the reduction actually shows up (round-1 intermediate snapshot —
superseded by round 3, see below)**: `git show`'s `--numstat` on the round-1
commit, restricted to the three files that remain on the still-conflicts
list because upstream independently touches them too (so removing our footprint
doesn't remove the conflict, only shrinks what has to be reconciled inside it):

| File | Net lines (round 1) |
|---|---|
| `src/CrossPointSettings.h` | −79 (5 insertions, 84 deletions) |
| `src/CrossPointSettings.cpp` | −11 (0 insertions, 11 deletions) |
| `src/SettingsList.h` | +56 (92 insertions, 36 deletions) — round 1 only moved the *fields* off `CrossPointSettings`; the `Dynamic*` row *definitions* still lived in `SettingsList.h`, which is why this file grew instead of shrinking. **This was temporary**: round 2 (below) moved those row definitions out too, and `SettingsList.h` ends Phase B with zero Midad-specific executable lines — see "Phase B round 2" for the corrected numbers. Kept here only as a historical record of the intermediate state; do not read this row as Phase B's final result. |

`src/CrossPointState.h`/`.cpp` were untouched this phase (none of the migrated
fields lived there, confirmed in the original audit) and still conflict for
reasons unrelated to Phase B.

**Remaining settings-related conflict files** (still conflicting, not
addressed by Phase B, tracked for a future phase): `src/CrossPointSettings.h`,
`src/CrossPointSettings.cpp`, `src/CrossPointState.h`, `src/CrossPointState.cpp`,
`src/SettingsList.h`, `src/activities/settings/SettingsActivity.h`,
`src/activities/settings/SettingsActivity.cpp`, `src/network/CrossPointWebServer.cpp`
(the settings web API), `src/network/html/SettingsPage.html`. None of these
were expected to reach zero in Phase B — they all carry either genuine Tier-1
integration surface (the settings *mechanism* itself) or upstream's own
independent, unrelated edits to the same files.

**What was deliberately NOT moved** (see the migration inventory above):
`bleEnabled` (not purely a settings-page field — also the live radio switch
`AppsActivity` special-cases) and `darkModeEnabled` (upstream has a parallel
`screenInverted` concept worth reconciling on its own, not a clean migration
to a Midad-only store).

**Verification**: `pio run -e default` (success), `pio check` (no defects),
`bin/clang-format-fix` (clean), `pio run -e simulator` (success, live-verified
the migration as described above), `ctest` (112/112 passed). `pio run -e
simulator_x3` currently fails, but on a pre-existing, unrelated line
(`HalClock::isSystemTimeValid` missing from that environment's own HAL stub,
`EpubReaderActivity.cpp:363`, untouched by this phase) — not a regression from
this change. On-device physical Settings/Apps-screen verification is flagged
for hardware testing, per this repo's usual simulator/hardware split.

## Phase A results: the real baseline, not an estimate

Ran the actual `Update from CrossPoint` action against `develop` on
2026-08-11 (`dry_run: true`, run
[31481945288](https://github.com/sfoulad/midad-by-foulad/actions/runs/31481945288)).
This is a genuine `git merge --no-ff` of `crosspoint-reader/develop` @
`2cac5971` into a disposable branch off `develop` @ `dc65c0f0` — not a
sample of individual commits, the real thing the whole plan is about.
`develop` itself was never touched (confirmed: still at `dc65c0f0`
afterward); the merge attempt happened on a throwaway branch and was
aborted on conflict, exactly as designed.

**Result: 158 conflicts** — 107 content, 36 modify/delete, 14 add/add, 1
rename/delete. This is today's true number; the individual-commit-sampling
audit below undercounted because a real merge accumulates every file both
sides touched across the *entire* divergent history at once, not just the
commits sampled for that audit.

**Confirms the tier predictions below directly** — every Tier 1/2 file
named in the per-file audit shows up in the real conflict list:
`lib/Epub/*`, `lib/GfxRenderer/*`, `EpubReaderActivity`/`EpubReaderMenuActivity`,
`SettingsList.h`, `CrossPointSettings.*`, `main.cpp`, `OpdsBookBrowserActivity.cpp`,
`CrossPointWebServer.cpp`, `RecentBooksActivity.cpp`, `HomeActivity.cpp`,
`BaseTheme.cpp`, and `WifiSelectionActivity.cpp/.h` (the parallel-implementation
candidate flagged below) all conflict, exactly as predicted.

**New findings this run surfaced that the sampling missed:**

- **Nearly every translation file conflicts** (`modify/delete` for most
  languages, `add/add` for `arabic.yaml`) — confirms the earlier
  session-level finding (translation files individually showed the same
  `modify/delete` pattern) but at full scale: this is not a handful of
  files, it's essentially the whole `lib/I18n/translations/` directory.
- **`src/JsonSettingsIO.cpp/.h`**: `modify/delete` — **the direction is the
  opposite of an earlier draft of this section**. Verified directly:
  `src/JsonSettingsIO.h` exists in our `develop` (confirmed via `git show
  origin/develop:src/JsonSettingsIO.h`) and does **not** exist in the
  `upstream` mirror. So upstream deleted it, we still have a modified copy —
  not the other way around. Investigated why we still have it: `grep` for
  every call site shows only two callers left, both bookmark I/O
  (`EpubReaderActivity.cpp:2708/2760`, `EpubReaderBookmarksActivity.cpp:40/78`
  calling `JsonSettingsIO::loadBookmarks`/`saveBookmarks`) — the
  `saveSettings`/`loadSettings` half of the file is dead code, unused since
  our own PersistableStore migration (task history: "Migrate
  CrossPointSettings/State off JsonSettingsIO onto PersistableStore").
  Upstream's own equivalent migration (`63eda54e`, "chore: Migrate
  Settings/State onto PersistableStore" — the same commit, independently
  built, matching our task title almost word for word) went one step
  further and *also* replaced bookmark persistence: it deleted
  `JsonSettingsIO.cpp/.h` entirely and added `src/util/BookmarkFile.cpp/.h`
  (`BookmarkFile::load(bookPath, ...)`/`save(bookPath, ...)`, confirmed in
  its diff) as the dedicated replacement. **This is a convergence
  opportunity, not a permanent divergence**: adopting `BookmarkFile` (or
  building our own equivalent) for our two remaining bookmark call sites
  would let us delete `JsonSettingsIO.cpp/.h` outright, closing this
  conflict completely. Worth pulling into the Settings-extraction phase
  (B) rather than treating as unfixable.
- **`src/CrossPointState.cpp/.h`** conflicts too — wasn't in the original
  per-file sample; add to the Tier 1 settings-system list.
- **Four genuine "add/add" surprises — both sides independently built the
  same thing**, each a real Phase G (adopt-upstream-or-contribute-ours)
  candidate, not just `WifiSelectionActivity`:
  - `src/ReaderFontSizes.cpp/.h` — matches upstream's own `#2720`
    "point-size font selection," the same feature we built independently
    (task history: "Phase 7: point-size font selection, fork-designed
    port"). Worth a real diff-and-compare, not just picking a side blind.
  - `lib/Memory/BuildScratch.h`, `lib/Serialization/BufferedFile.h` — both
    sides built overlapping memory/serialization scratch utilities.
  - `lib/EpdFont/builtinFonts/source/NotoSansArabic/*` (the font source
    files themselves) — upstream has landed its own Arabic font support
    (matches their `#2596`/`#2599` "Arabic glyphs in built-in UI fonts"/
    "Arabic translation" work found in the commit log), independently of
    ours. Given this project's whole reason for existing leans on Arabic
    support, this specific conflict deserves early, careful attention in
    Phase F/G — it's possible upstream's approach has converged with or
    could replace parts of ours.
  - `src/util/DictZip.cpp/.h`, `src/activities/reader/DictionaryDefinitionActivity.*`,
    `DictionaryWordSelectActivity.*` — both sides built dictionary-lookup UI
    independently. Consistent with Dictionary's existing Tier 4 status
    (deliberate fork) — not a signal to reconsider that, just confirms it.

**What this run actually validated, precisely** (this run's own status shows
as a workflow *failure* — that's expected and correct, not a malfunction:
the `report-conflict` job intentionally `exit 1`s when conflicts are
detected, specifically so a real conflict can't silently look like success
in the Actions UI):

- the `upstream` mirror updates correctly
- a real merge attempt runs correctly
- conflict detection works
- the exact conflicting-file list is reported correctly
- `develop` is left untouched
- the automation correctly aborts rather than attempting any auto-resolution

**What this run did *not* exercise**: because this merge conflicted, the
`ci` job (`workflow_call` into the reusable `ci.yml`) and the `open-pr` job
were both skipped by their `if: needs.sync.outputs.conflict == 'false'`
condition. **The clean-merge → CI → PR path has not been run yet.** That
still needs a real exercise — either a future sync that happens to merge
cleanly, or a deliberate test (e.g. temporarily pointing `upstream_ref` at
an older upstream commit close enough to `develop` to merge clean) — before
it can be called validated.

The 158-conflict number is the number Phases B–G exist to bring down;
re-run this same workflow after each phase to confirm it's actually
shrinking, not just moving around (see "Running the audit yourself," which
now doubles as "running the real check" via the Action itself rather than
only the commit-sampling script).

---

## The goal, stated precisely

Today, taking an upstream CrossPoint update means: fetch, find what's new,
figure out which commits are safe, hand-port or cherry-pick individually,
fix whatever our own divergence breaks, test, repeat. That doesn't scale and
doesn't use the tooling GitHub already provides for exactly this situation.

The target workflow is:

> CrossPoint releases an update → sync/merge upstream → CI builds/tests →
> done.

Not:

> CrossPoint update → review every commit → manually port → fix diverged
> files → repeatedly test and repair.

This is achievable specifically because `git merge` only produces conflicts
on files/hunks **both** sides touch. If Midad's footprint on CrossPoint-owned
files shrinks to a small, stable set of integration hooks, a real
`git merge upstream/develop` becomes mechanical almost everywhere, and the
only work left is: resolve the handful of hooks that conflict, then verify
nothing broke. That second part — verify, don't re-review — is the other
half of the mindset shift this doc is asking for: **a clean `git merge` with
no conflicts is not an invitation to re-examine whether we like upstream's
implementation better than our old one. If it merges clean and the build/test
gate passes, it ships. Only intervene where something actually breaks.**

## 1–4. Audit: what's actually mixed into CrossPoint-owned files today

Method: `git diff --numstat <merge-base> HEAD` restricted to paths that
existed at the merge-base (`1f5669a0`, 2026-07-11) — i.e., true Midad edits
to files we started with, filtered of noise (generated font headers under
`lib/EpdFont/builtinFonts/`, translation YAML, binary test fixtures). Full
methodology in "Running the audit yourself," below, so this can be re-run
after every phase.

### Tier 1 — genuinely hard to separate (core rendering/parsing, Arabic-woven)

| File | Changed lines | Why it's hard |
|---|---|---|
| `lib/GfxRenderer/GfxRenderer.cpp/.h` | 1316 + 256 | 145 Arabic/kashida/bidi mentions vs. 7 generic-refresh mentions — the divergence is dominated by Arabic text measurement/shaping, not the ported generic perf work (which is small and likely already close to mergeable). |
| `lib/Epub/Epub/ParsedText.cpp`, `Section.cpp`, `parsers/ChapterHtmlSlimParser.cpp`, `Epub.cpp`, `BookMetadataCache.cpp`, `blocks/*` | ~900 combined | Core EPUB parsing/layout; Arabic bidi ordering, kashida justification planning, niqqud anchoring change *how every word is measured and positioned*, per the existing audit in this doc's prior revision. |
| `src/activities/reader/EpubReaderActivity.cpp/.h` | 1367 + 137 | The reader activity itself: per-book settings, Arabic UI, dark mode, sync hooks, Quick Resume handling all land here. |
| `src/activities/reader/EpubReaderMenuActivity.cpp/.h` | 737 + 86 | Reader menu: text settings, Apps integration (Pomodoro/Tasbih shortcuts), Arabic-specific menu items. |
| `src/CrossPointSettings.h/.cpp`, `src/SettingsList.h` | 294+376+579 | The settings system — see the dedicated section below; this one *is* fixable, tracked separately from the true Tier 1 set. |
| `src/main.cpp` | 765 | Boot/loop orchestrator every subsystem hooks into — see "integration points" below; largely legitimate hooks, not core rewrites, but large in raw line count. |

This tier already has a real, evidence-backed reduction plan from the prior
revision of this doc (extract kashida planning and any other self-contained
Arabic functions into `lib/ArabicShaper/`, following the precedent already
set by `lib/ArabicShaper/` itself and `lib/MiniBidi/` both already being
separate libraries the shared files call into rather than reimplement). That
plan stands and is folded into the phased plan below. It will not reach zero
— CrossPoint's own future layout/perf work will keep landing in these same
files — but it can shrink materially.

### Tier 2 — Midad/Foulad features embedded in shared files (extraction candidates)

| File | Changed lines | What's actually in it |
|---|---|---|
| `src/activities/browser/OpdsBookBrowserActivity.cpp/.h` | 1171 + 194 | Foulad eBooks device tracking, BLE `book.fetch` integration, crosspoint-sync hooks, mixed into the core OPDS browser. |
| `src/network/CrossPointWebServer.cpp/.h` | 664 + 64 | Mostly **not** branding (only 2 Foulad/Midad string literals) — the bulk is genuinely new feature code: `/dictionaries`, `/api/dictionaries/*` (dictionary web-management, `handleDictionariesPage()`/`handleDictionaryList()`/`handleDictionaryUpload()`/`handleDictionaryDelete()`), all defined as methods directly on the shared class. |
| `src/activities/home/RecentBooksActivity.cpp/.h` | 508 + 50 | Apps pseudo-tiles (Quran/Games/Gym/Tasbih/Stopwatch/Pomodoro pinned entries) mixed into the core "recent books" grid logic. |
| `src/activities/home/HomeActivity.cpp/.h` | 359 + 86 | BLE hold-Confirm-gesture entry point, cover-scan tweaks. |
| `src/network/HttpDownloader.cpp/.h` | 669 + 99 | Needs a closer split: some of this is genuinely generic (heap-safety/TLS fixes potentially upstream-worthy), some is Foulad-eBooks-specific (multipart uploads for font conversion, device-log posting). Audit at extraction time, don't assume it's one or the other. |
| `src/components/themes/BaseTheme.cpp/.h`, `lyra/LyraTheme.cpp/.h` | 221+43, 224+9 | Not purely additive the way the earlier audit assumed — real modifications live here too, not just the separate `foulad/` directory. Needs its own look: how much is Midad branding/UI tweaks vs. shared theme-engine capability CrossPoint would want too. |
| `src/images/Logo120.h/.png` | 191 + binary | Pure branding asset. Trivially separable — a resource swap point, not logic. |
| `src/fontIds.h` | 69 + 49 | Font ID table additions for new fonts — likely purely additive; low risk either way. |

### Tier 3 — legitimate, minimal integration hooks (keep, don't fight)

| File | Changed lines | Nature |
|---|---|---|
| `src/activities/ActivityManager.cpp/.h` | 203 + 60 | `goToXxx()` navigation entry points for BLE, Apps, Bluetooth, etc. — genuinely a hook pattern, not a rewrite, even though the line count looks large (each new activity needs one method). |
| `lib/hal/HalGPIO.cpp` | 117 (mostly Phase 3, 2026-08-11) | Hardware detection integration — see Phase 3 case study below for why even this small a footprint still needs real care. |
| `src/main.cpp`'s ~39 BLE-specific lines (subset of its 765) | small | Call sites and branches around `BlePeripheralManager`/`bleAllowedNow` — not rewrites of boot logic CrossPoint owns. |

### Tier 4 — deliberate, already-decided fork points (leave alone)

- **Dictionary** (`src/util/Dictionary.cpp/.h` — 672+188 lines *deleted* relative to merge-base): fully replaced by our own `DictionaryStore`. Not a migration target; evaluate individual upstream dictionary commits for ideas only, per the existing decision.
- **`roundedraff` theme** (433+112 lines deleted): we simply never adopted it. Free to pull back in via merge if ever wanted — zero current cost since nothing touches it.
- **`lib/EpdFont/builtinFonts/*`** (many thousands of lines deleted): flash-embedded font glyph data replaced by the SD-card font system. A real architecture divergence, but already-decided and isolated to font loading, not spread through rendering logic.

### Real candidates for the *opposite* direction — adopt upstream's implementation, or contribute ours

- **`WifiSelectionActivity.cpp`**: both sides independently built auto-connect-saved-networks + hidden-SSID support (upstream's own PR #2189; ours, Phase 1.5, already shipped). 49 auto-connect/hidden-SSID mentions on our side, 34 on upstream's, for the same feature. This is not Foulad-specific — it's generically useful to any CrossPoint device. **Candidate: evaluate replacing our implementation with upstream's on the next sync**, rather than extracting ours to a Midad module — closes the gap entirely instead of permanently carrying a parallel implementation.
- **Small, already-identified generic fixes**: the `FOOTNOTE_HREF_LEN` 96→256 fix (hand-applied 2026-08-11, upstream #2722) and similar isolated correctness fixes are exactly the kind of thing worth a small upstream PR — they're not Midad-specific at all, we just found them first or fixed them independently. Track these as "upstream contribution candidates" as they're found, rather than only ever pulling upstream's version.

## 4 (continued). Minimum permanent CrossPoint integration surface

Based on the above, the honest floor — files that will **always** need a
Midad-side touch, even after full extraction work, because they're either
the literal entry point every subsystem hooks into or core rendering logic
Arabic support cannot be fully lifted out of:

1. `src/main.cpp` — boot sequence and main loop call sites (small hooks, not rewrites, once Apps/BLE-specific logic is fully moved out where it hasn't been already).
2. `src/activities/ActivityManager.cpp/.h` — one `goToXxx()` per Midad-owned activity.
3. `lib/GfxRenderer/GfxRenderer.cpp/.h`, `lib/Epub/Epub/{ParsedText,Section}.cpp` — a residual, hopefully-shrinking set of Arabic integration points and additive serialization fields, after the extraction plan below.
4. `CrossPointSettings.h`/`SettingsList.h` — down to zero *new* touches going forward once the settings-page pattern (below) is applied everywhere; existing fields that must stay flat (few) still route through here.

Everything else audited above should end up in Tier 2/3/4, not this list.

## The settings system: already has a working pattern, needs applying everywhere

Confirmed directly: **BLE has zero footprint in `CrossPointSettings.h` and
zero entries in `SettingsList.h`** — no field, no `SettingInfo` row.
`BluetoothActivity` is reached by holding Confirm on Home, a dedicated entry
point bypassing Settings entirely, and owns its own state. OPDS settings
follow a related pattern: `OpdsSettingsActivity` is a fully separate
activity, reached from `OpdsServerListActivity`, never appearing in
`SettingsList.h`.

**Standard going forward:** any new Midad-specific configuration gets its
own activity + its own `PersistableStore` (or a field on `CrossPointState`
for pure runtime state), reached contextually from wherever the feature
lives — never a new entry in the shared array.

**What's currently mixed in and needs migrating** (audited 2026-08-11,
checked field-by-field against upstream's `CrossPointSettings.h`):

- **Clean migration candidates, zero upstream equivalent, already tied to
  the fully-separate `src/activities/apps/`/`src/activities/games/`
  module**: `quranEnabled`, `gamesEnabled`, `rssEnabled`, `tasbihEnabled`,
  `stopwatchEnabled`, `pomodoroEnabled` (+ `pomodoroFocusMin`/
  `ShortBreakMin`/`LongBreakMin`), `gymEnabled`/`gymWeightUnit`,
  `debugLoggingEnabled`. Consumers span `AppsActivity.cpp`,
  `EpubReaderMenuActivity.cpp`, `RecentBooksActivity.cpp`, `QuranBook.*`,
  `main.cpp`, and — importantly — `FouladDeviceTracking.cpp`'s
  `addSettingsReport()`/`applySettingsFromServer()`, which is hand-written
  field-by-field JSON mapping (not generic serialization), so the wire
  contract (JSON key names) can stay identical while the storage backing
  changes. Target: a new `MidadAppSettings` store next to
  `src/activities/apps/`, migrated the same way `KOReaderCredentialStore` is
  already handled independently inside `applySettingsFromServer()` (its own
  `changed` flag, its own `saveToFile()`) — i.e. there's already a working
  precedent for a second store inside that same function, not a novel
  pattern.
- **Not a clean migration — flag, don't move the same way**: `darkModeEnabled`
  has no upstream name-match, but upstream has `screenInverted` (independently
  built, same concept, different field and likely different logic, later
  extended with image-polarity preservation upstream doesn't share with us).
  Moving it to a Midad store wouldn't reduce conflict risk — it would make it
  harder to ever reconcile with upstream's version. Leave in place; track as
  its own "should we adopt `screenInverted` instead" question, same bucket as
  the WiFi auto-connect candidate above.

This migration is scoped and moderate-effort (touches 7+ files for
`quranEnabled` alone) — sequence it into the phased plan below, not as a
same-day change. **Done as of Phase B (PR #136) — see "Phase B results" above.**

## Arabic/EPUB core: shrink the surface, don't accept it as fixed

(Carried forward from the prior revision of this doc — still the right
plan, now positioned as Tier 1's mitigation rather than a standalone
concern.)

`lib/ArabicShaper/` (glyph shaping) and `lib/MiniBidi/` (bidi reordering) are
**already** extracted into standalone libraries the shared files call into.
What remains inline is narrower than "everything Arabic": primarily the
kashida-justification planning function in `ParsedText.cpp` (confirmed
self-contained — calls `ScriptDetector`/`renderer`, not interleaved
character-by-character with the general path) and a handful of
`ScriptDetector::containsArabic(...)` branches elsewhere. Audit
`ParsedText.cpp`/`Section.cpp`/`ChapterHtmlSlimParser.cpp` for every such
self-contained function and extract it the same way, leaving a single call
site behind. What can't move — additive serialization fields like
`kashidaExtraPx[wordCount]` — is low conflict risk by nature (new fields,
not rewrites of existing ones), same class as `BoardProfile` gaining a new
opt-in field in the freeink-sdk work.

## 5. Branch structure and GitHub automation

```
crosspoint-reader/crosspoint-reader (real upstream, GitHub)
        │  fetch/mirror, no writes
        ▼
  upstream            <- exact mirror of crosspoint-reader/develop, ZERO Midad commits, force-updated by automation only
        │
        │  git merge upstream (real merge, not rebase, not cherry-pick)
        ▼
  sync/crosspoint-<date-or-tag>   <- ephemeral, created per sync run, CI validates here
        │
        │  PR, human review of *hooks only*, CI green
        ▼
  develop             <- Midad's real branch: CrossPoint + thin Midad overlay
```

- **`upstream`**: tracks `crosspoint-reader/crosspoint-reader`'s `develop`
  (or whichever branch we decide is the sync source — confirm with the
  team) exactly. No Midad commits ever land here. Kept current by
  automation (`git fetch upstream && git push origin upstream:upstream
  --force`), safe to force-push since it's a pure mirror with no local
  history of its own.
- **`develop`**: Midad's actual branch, receiving `git merge upstream`
  periodically — a real merge commit, not a squash, not a rebase, so the
  full CrossPoint history stays intact and future merges have correct
  common-ancestor information (this is what makes merges keep getting
  *easier* over time, not harder — rebasing or squashing here would throw
  that away and make every future merge re-derive conflicts git could
  otherwise already know are resolved).
- **`sync/crosspoint-<id>`**: a disposable branch the automation creates for
  each sync attempt, merges `upstream` into (based on current `develop`),
  runs the full build/test gate on, and opens a PR from. Never merged
  directly — always via reviewed PR into `develop`.

### GitHub Action: "Update from CrossPoint"

Manually triggered (`workflow_dispatch`), roughly:

1. **Fetch upstream** — `git fetch crosspoint-reader develop` (or configured source ref).
2. **Update the mirror** — fast-forward (or force-update) the `upstream` branch to match exactly; push.
3. **Create the sync branch** — branch `sync/crosspoint-<run-id>` off current `develop`, `git merge upstream` into it (real merge, `--no-ff`).
   - If the merge has conflicts: commit nothing further automatically. Report which files conflicted, open a draft PR with the conflict markers in place, and stop — per the explicit instruction, **do not attempt a broad redesign in automation**. A human resolves the hook conflicts (expected to be small, per the tiers above) and pushes the resolution to the same branch.
   - If the merge is clean: proceed directly to the build/test gate.
4. **Build/test gate** — reuse the existing CI jobs this repo already has (`pio run` for `default`/`gh_release`/`gh_release_rc`/`slim`, `pio check` cppcheck, `clang-format` check, the `simulator`/`simulator_x3` targets). Nothing new to invent here — same gate as any other PR, just run against the sync branch.
5. **Open the PR** — `sync/crosspoint-<run-id>` → `develop`, auto-generated description listing: the upstream commit range pulled in, whether the merge was clean or had conflicts (and which files), and the CI results. Human review from here is exactly what the "minimize hooks, don't second-guess a clean merge" principle is for — reviewing a small, predictable diff, not re-auditing the whole update.
6. **What CI cannot cover**: real-hardware boot/flash verification (the Phase 3 soft-brick lesson — a clean simulator build does not guarantee a clean boot when `freeink-sdk`/HAL files are in the merged range). The PR description should flag when the merge touched `freeink-sdk`, `lib/hal/*`, or display/power code, as a explicit "needs a human to flash and watch the boot log" callout — automation cannot self-certify this part.

This is genuinely close to the "nearly one-click" goal for the common case
(no conflicts, no HAL changes): trigger → PR appears green → merge. The
remaining human steps are exactly the cases that need judgment: hook
conflicts, and hardware verification.

## 6. Phased migration plan

Ordered by risk/value, each phase independently shippable and verifiable —
none of this risks the current firmware because `develop` doesn't change
until a phase's own PR merges, and every phase gets the same
build+flash+boot-log verification this project already practices.

**Phase A — Branch scaffolding (no code changes, low risk).**
Create the `upstream` mirror branch. Stand up the "Update from CrossPoint"
Action in dry-run/report-only mode first (steps 1–4, skip auto-PR) to see
what today's real merge conflict set looks like end to end, before trusting
it to open PRs. This alone answers "how much would a real `git merge
upstream/develop` conflict right now" with certainty, replacing the
individual-commit audit numbers in this doc with ground truth.

**Phase B — Settings extraction. Done, PR #136 (pending merge).**
Built `MidadAppSettings`, migrated the 11+1 fields listed above, updated
`FouladDeviceTracking.cpp`'s report/apply logic, removed the fields from
`CrossPointSettings.h`/`SettingsList.h`. Also adopted upstream's `BookmarkFile`
module, closing the `JsonSettingsIO.cpp/.h` conflict outright. See "Phase B
results" above for the full writeup, including the honest finding that the
real merge-conflict *file count* didn't drop (157 → 158 — 2 files eliminated,
3 introduced, but the 3 new ones are a near-mechanical add/add pair plus one
deliberate 2-line visibility change, nowhere near the old modify/delete
conflict's complexity) even though `CrossPointSettings.h` shrank by 79 lines
and `CrossPointSettings.cpp` by 11. The metric to watch going forward is which
files *upstream can still independently edit into a conflict*, not a raw
count — Phase C's file-count delta should be read the same way.

**Phase C — OPDS/device-tracking extraction.**
Move Foulad-specific logic out of `OpdsBookBrowserActivity.cpp` into a
Midad-owned module it calls into at defined hook points (mirroring how
`FouladDeviceTracking.cpp` already exists as a separate file — the gap is
that `OpdsBookBrowserActivity.cpp` itself still contains inline device-
tracking/book.fetch glue rather than just calling out to it). Verify OPDS
browsing, downloads, and BLE `book.fetch` all still work against the real
Foulad eBooks server.

**Phase D — Web server extraction.**
Move the dictionary-management endpoints (`/dictionaries`,
`/api/dictionaries/*`) out of `CrossPointWebServer.cpp` into their own
handler class registered via a hook, rather than methods on the shared
class. Verify the dictionary web UI (upload/list/delete) still works.

**Phase E — Theme audit.**
Determine how much of `BaseTheme.cpp/.h` and `LyraTheme.cpp/.h`'s
divergence is Midad branding (extractable to `foulad/`) vs. shared
theme-engine capability worth keeping core (or contributing upstream).
Smaller than B–D; do after them once the pattern is well-practiced.

**Phase F — Arabic/EPUB extraction (highest effort, do last, never fully "done").**
Audit `ParsedText.cpp`/`Section.cpp`/`ChapterHtmlSlimParser.cpp` for every
self-contained Arabic function (kashida planning confirmed as the first
target) and extract to `lib/ArabicShaper/`. Expect this to be iterative —
each sync will likely surface a few more extraction opportunities rather
than being solvable in one pass. Track progress via `Section.cpp`'s own
FORK VERSION HISTORY comment convention, same as today.

**Phase G — Upstream contribution pass.**
For the "adopt upstream's / contribute ours" candidates identified above
(WiFi auto-connect being the clear first case) and any small generic fixes
found along the way (`FOOTNOTE_HREF_LEN`-style), decide per-case: replace
ours with upstream's on the next merge, or open a PR against
`crosspoint-reader/crosspoint-reader` with ours. Either outcome shrinks the
permanent fork delta; this phase is ongoing, not a one-time pass.

**Phase H — Flip the default workflow.**
Once B–D have meaningfully reduced the conflict surface (validated by
Phase A's dry-run numbers improving on a re-run), make "Update from
CrossPoint" the default way future updates land, with the individual-
commit-audit process from this doc's prior revision demoted to a fallback
for the (hopefully rare) cases the automated merge can't resolve cleanly.

## Running the audit yourself

```bash
git fetch upstream
BASE=$(git merge-base HEAD upstream/develop)
git diff --numstat "$BASE" HEAD -- $(git ls-tree -r --name-only "$BASE") \
  | grep -v "builtinFonts\|I18n/translations\|\.epub$\|\.png$\|\.bmp$\|\.jpg$" \
  | awk '{tot=$1+$2; print tot, $0}' | sort -rn | head -40
```

Re-run after each phase to confirm the divergence numbers are actually
shrinking, not just moving around.

## Prior sync-session findings (kept for reference)

These are concrete, already-verified lessons from the 2026-08-11 session
that still apply regardless of the branch-structure change above — they're
about *how* to safely take an upstream change, not *which* branch it lands
on.

- **A clean git-level merge is not automatically a functional merge.**
  Several individually-cherry-picked commits that session showed as "clean"
  produced an *empty* diff — meaning the fix was already present via
  independent work. Under the new merge-based workflow this matters less
  (a real `git merge` naturally no-ops on content that's already
  equivalent), but it's why "trust a clean merge, verify with tests" is the
  right instinct rather than "clean merge, therefore definitely new
  behavior to manually re-verify."
- **Prefer upstream's actual commit/implementation over hand-writing an
  equivalent**, confirmed the hard way in Phase 3: a hand-written port of
  upstream's X3/X4 panel-controller detection fix called the SDK's probe
  function *after* `SPI.begin()` instead of before, silently hanging the
  device at boot on real hardware — zero serial output, not even the ROM
  bootloader banner. `git log --oneline upstream/develop -- <path>` found
  the exact isolated upstream commits that already had the correct
  ordering; using them directly worked and came with a bonus fix (an X3
  gyro power-down bug) the hand-port would have missed entirely. Under the
  merge-based workflow this is even more true — accept upstream's
  implementation as-is on a clean merge rather than reconciling it with
  whatever we used to have there.
- **A `freeink-sdk` submodule bump is its own decision.** Prefer whatever
  commit upstream itself currently ships (`git ls-tree upstream/develop
  freeink-sdk`) over the newest available tip, which may carry unrelated,
  unvetted work — confirmed 2026-08-11 when upstream's own production pin
  was both safer and sufficient versus ~33 additional commits between it
  and origin/main's tip.
- **HAL/SDK/display changes need a human at a real device, always** — see
  the GitHub Action design's step 6. No amount of clean CI replaces watching
  a cold boot log on real hardware when the merge range touches this
  surface.
- **`FOOTNOTE_HREF_LEN` 96→256 (upstream #2722)** was hand-applied
  2026-08-11 with its own `SECTION_FILE_VERSION` bump, because the
  surrounding file had diverged too far for a clean pick at the time. Once
  Phase F reduces that divergence, re-verify this and similar small fixes
  actually merge cleanly going forward — they're exactly the kind of thing
  that should stop needing hand-application once the extraction work lands.
