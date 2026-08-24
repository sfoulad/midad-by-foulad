# OTA Signing: Production Key Ceremony

Status: **prepared, not executed.** No production key has been generated. This
document is the exact runbook for task #198 (provision the production OTA
signing key) and the first signed RC that follows it. It exists so the
ceremony can be executed deliberately, once, by the release maintainer --
not synthesized ad hoc under time pressure. See
`docs/ota-signing-key-management.md` for the design rationale (scheme, trust
anchor, custody policy, rotation limits) this runbook implements, and
`docs/ota-hardware-tamper-test-checklist.md` for the separate hardware
validation phase that follows (task #197, blocked on a disposable test
device -- out of scope here).

**Who runs this:** the release maintainer, on a machine they control, not an
automated agent. Every command below is written to be copy-pasted and run
by a human. Nothing in this document generates, transmits, or stores the
actual key material -- it only describes how to.

## What this ceremony touches

Every file, environment, workflow, and board this affects -- confirmed
against the actual repository state (not assumed from the design docs
alone), so this list can be used as a completeness check.

**Files that change:**
- `ota-signing-public-key.pem` (repo root) -- created, committed. Does not
  exist yet as of this writing.
- No other repository file changes as part of the ceremony itself --
  `scripts/sign_firmware.sh` and all three release workflows already
  reference this exact filename and the `OTA_SIGNING_KEY` secret name; they
  were wired up in advance specifically so the ceremony needs no code
  change, only the secret + the one file.

**GitHub repository state that changes:**
- Actions secret `OTA_SIGNING_KEY` -- created. Currently absent (confirmed:
  `gh secret list` shows only `CROSSPOINT_SYNC_TOKEN`).

**Workflows whose behavior changes the moment both of the above exist:**
| Workflow | Trigger | Boards built | What changes |
|---|---|---|---|
| `.github/workflows/auto-release.yml` | every push to `develop` touching firmware-affecting paths | `gh_release_rc` (X3/X4) only | `Sign firmware` step stops failing closed; publishes a pre-release automatically on qualifying pushes |
| `.github/workflows/release_candidate.yml` | manual (`workflow_dispatch`), only from a `release/*` branch | all 4 boards | Same -- this is the deliberate multi-board RC path, see Step 9 |
| `.github/workflows/release.yml` | push of a version tag | all 4 boards | Same -- **stable** release publishing; explicitly out of scope for this ceremony/Round 2 |

**Boards affected (all 4, identically -- one key signs all of them):**
| Board | Release env | RC env |
|---|---|---|
| X3 / X4 (dual-target build) | `gh_release` | `gh_release_rc` |
| Sticky (ESP32-S3) | `sticky-gh_release` | `sticky-gh_release_rc` |
| X4 Pro (ESP32-S3) | `x4pro-gh_release` | `x4pro-gh_release_rc` |
| PaperMono | `papermono-gh_release` | `papermono-gh_release_rc` |

All 8 of these environments (confirmed via `platformio.ini`) already carry
`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`/`CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME`
(inherited from `[base]`, applied to every env including dev/default) and
`-DOTA_SIGNING_BOOT_CHECK_ENABLED=1` (release/RC envs only -- enforced by
`scripts/check-ota-signing-flags.py` in CI, so a new board added later
without this flag fails CI rather than shipping silently unprotected). No
`platformio.ini` change is needed for the ceremony itself.

**Explicitly NOT touched by this ceremony:**
- `.github/workflows/ci.yml` (ordinary PR/branch builds never sign; stays
  unsigned by design, matching `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`).
- Any device firmware/source file -- `src/OtaSigningBootGuard.cpp` and the
  Kconfig flags are already in place from earlier work (FE-P3-OTA-SIGNING-CONFIG-001,
  PR #163) and need no change for the key to activate.
- Hardware -- nothing gets flashed by provisioning the key. Task #197
  (hardware tamper-test matrix) is a fully separate phase.

## Custody & recovery plan

This section is deliberately self-contained -- everything needed to
understand *why* the backup step matters, not just what command to run.

**Custody model:** two independent encrypted copies of the private key,
each behind a different access boundary, plus the live copy inside
GitHub's encrypted-secrets store (which itself is the only place the key
material is ever transmitted to). No copy of the unencrypted key persists
anywhere after Step 7.

| Component | Where it lives | Who/what can access it | Read-back possible? |
|---|---|---|---|
| Live signing copy | GitHub Actions encrypted secret `OTA_SIGNING_KEY` | Only workflow runs on this repo, injected into the `Sign firmware` step's environment for that step alone | No -- GitHub secrets are write-only; `gh secret set` can overwrite but nothing can read the value back out |
| Backup copy #1 | `age`\/`gpg`-encrypted file (`midad-ota-signing-key-production.pem.age`) | Whoever the release maintainer grants access to, via a password manager's secure-note file attachment (matches existing custody practice for other release credentials) | Only with the separately-stored passphrase |
| Backup copy #2 | Same encrypted file, second independent location | An offline, encrypted USB drive kept in physical custody, not networked | Only with the separately-stored passphrase and physical access |
| Passphrase | Stored separately from both encrypted copies (e.g. the password manager's own vault, not attached to the file) | Release maintainer | N/A |
| Unencrypted working copy | Ceremony machine, briefly (Steps 1-6) | Whoever is running the ceremony | Deleted in Step 7; never leaves that machine |

**Why two independent backups, not one:** a single backup location is a
single point of loss (drive failure, account lockout, etc.) for a key that
-- per `docs/ota-signing-key-management.md`'s "Key rotation design" -- has
**no remote recovery path** if lost. Two independent locations means one
failing doesn't mean losing the key.

**Recovery plan if the key is compromised:** stop signing with it
immediately, then follow `docs/ota-signing-key-management.md`'s
"Compromised-key emergency process" section verbatim (Rotation option 1 --
the bridge-release procedure -- executed as an emergency operation, not a
routine one). That section is authoritative; this document doesn't
duplicate its steps.

**Recovery plan if the key is lost (nobody has it, including backups):**
per the same design doc's "Lost key" section, **there is no remote
recovery** -- devices that already trust this key can only be recovered by
physical re-flash (Option 2). This is precisely why the two-independent-
backup custody model above exists: it is designed to make this scenario
very unlikely, not to provide a way out of it after the fact.

**How we verify no private-key material leaks:**
- **Shell history / process list**: Step 5's `gh secret set ... < file`
  form passes the key via stdin redirection, never as a CLI argument --
  nothing sensitive appears in `ps` output or, ordinarily, in shell
  history. Step 7 includes an explicit `history | grep` check as a
  best-effort backstop.
- **Git**: the `.gitignore` patterns added in this same PR
  (`*.signing-key.pem`, `devkey-*.pem`, the exact production-key working
  filename) mean `git status`/`git add -A` cannot silently pick up the key
  file if the ceremony happens to run inside the repo directory (it
  shouldn't -- Step 1 uses `~/ota-signing-ceremony`, outside the repo
  entirely -- but the gitignore is a second, independent backstop).
- **CI logs**: `scripts/sign_firmware.sh` writes the key to a `mktemp` file
  that its own `trap ... EXIT` deletes, and never echoes the key value at
  any point (confirmed by reading the script -- no `set -x`, no `echo
  "$OTA_SIGNING_KEY"` anywhere). GitHub Actions also automatically redacts
  any secret value that happens to appear verbatim in log output, as a
  second, independent layer.
- **GitHub secret itself**: cannot be read back via the API or UI by
  anyone, including the repo owner -- only overwritten. This is a GitHub
  platform guarantee, not something this project's code enforces.
- **Post-ceremony confirmation**: after Step 7, run
  `git status` (should show nothing untracked matching key-file patterns)
  and `history | grep -i signing-key` (should show only file*path*
  references, never key bytes) as a final manual check before considering
  the ceremony complete.

## Public-key integration plan

What committing `ota-signing-public-key.pem` (Step 6) does and does not do,
stated explicitly since this is the step most likely to be misunderstood:

- **Does**: gives CI a value to check its own signing output against
  before publishing (`espsecure.py verify_signature` inside
  `sign_firmware.sh`) -- catches a wrong/corrupt key or tooling regression
  at signing time, before a device ever sees the artifact.
- **Does NOT**: change what any device trusts. Per
  `docs/ota-signing-key-management.md`'s "Where the runtime trust anchor
  lives" section, that trust anchor is read live from the *currently
  running app's own appended signature block* on each device -- this file
  plays no role in that check. A device only starts trusting this key once
  it actually installs a release signed with it.
- **No firmware/source change required**: `src/OtaSigningBootGuard.cpp` and
  every release/RC `platformio.ini` environment are already wired to expect
  a signed image; committing this one file is the only integration step.
- **Safe to be public**: this is the *public* half; committing it is
  permanent (visible in git history forever) and intentional -- unlike the
  private half, which must never be committed anywhere, ever.

## Pre-key automated validation (already running, confirmed live)

This is not a future action item -- it already exists and is exercised on
every push to `develop`, verified in this session against the actual CI run
for the PR #181 merge commit (`6b440390`, job "Validate lock (Python
3.13/3.14)"): all 5 `SignFirmwareRoundTripTest` cases in
`scripts/tests/test_sign_firmware.py` ran for real against real
`espsecure.py 4.12.0` (not skipped -- that job installs it from
`scripts/requirements-ci.lock`) and passed:
- `test_valid_signature_is_accepted`
- `test_wrong_key_is_rejected`
- `test_unsigned_firmware_is_rejected`
- `test_tampered_firmware_is_rejected`
- `test_signing_fails_closed_if_key_file_write_fails`

Each generates its own throwaway keypair via the *exact* same
`generate_signing_key --version 2 --scheme rsa3072` command this ceremony's
Step 1 uses, and exercises `sign_firmware.sh` -- the real, shipped script,
not a reimplementation. This is the mechanical proof that the ceremony's
commands work end-to-end, obtained without ever touching the real key. No
additional pre-key validation is needed beyond this existing, CI-enforced
suite.

## Pre-ceremony checklist

- [ ] A machine you trust with the key for the few minutes it exists
      unencrypted on disk -- ideally not a shared or cloud-synced machine
      (disable iCloud Drive / Dropbox / OneDrive sync on whatever directory
      you use, or work outside all synced folders entirely).
- [ ] Network can be disconnected during key generation (not required --
      `espsecure.py generate_signing_key` doesn't touch the network -- but
      disconnecting removes any doubt).
- [ ] `esptool` 4.12.0 installed (`pip install esptool==4.12.0`), matching
      the exact version pinned in the release workflows
      (`scripts/tests/test_sign_firmware.py`'s round-trip suite already
      exercises this exact version against `sign_firmware.sh`).
- [ ] `gh` CLI authenticated with write access to
      `sfoulad/midad-by-foulad` (confirmed working in this session: `push`
      and `admin` permissions both present).
- [ ] A decision made on **backup custody** before generating anything --
      see "Backup" below. Don't generate first and figure out where it goes
      after.
- [ ] Read `docs/ota-signing-key-management.md`'s "Key rotation design"
      section once more before proceeding: there is **no remote key
      rotation** under this scheme. A lost or compromised key is a
      truck-roll-scale incident, not a rotate-and-move-on one. Custody is
      the only real control.

## Step 1 -- Generate the keypair

```bash
mkdir -m 700 -p ~/ota-signing-ceremony
cd ~/ota-signing-ceremony

espsecure.py generate_signing_key --version 2 --scheme rsa3072 \
  --keyfile midad-ota-signing-key-production.pem
chmod 600 midad-ota-signing-key-production.pem
```

This is the exact subcommand/flag syntax `scripts/tests/test_sign_firmware.py`
already round-trips in CI against real `espsecure.py 4.12.0` -- no untested
syntax variant.

## Step 2 -- Extract the public key

```bash
espsecure.py extract_public_key --version 2 \
  --keyfile midad-ota-signing-key-production.pem \
  --output ota-signing-public-key.pem
```

This is the file `scripts/sign_firmware.sh` and all three release workflows
already reference by this exact name; it is **not** the OTA-install-time
trust anchor (that's read live from the running app's own signature block --
see `docs/ota-signing-key-management.md`), only the self-check CI runs before
publishing.

## Step 3 -- Self-verify before trusting the output

Matches the same self-check `sign_firmware.sh` performs on every real
release -- run it once by hand first, against throwaway data, so a tooling
problem surfaces here and not on the first real release:

```bash
head -c 65536 /dev/urandom > selftest.bin
espsecure.py sign_data --version 2 --keyfile midad-ota-signing-key-production.pem \
  --output selftest-signed.bin selftest.bin
espsecure.py verify_signature --version 2 \
  --keyfile ota-signing-public-key.pem selftest-signed.bin
# Expect: "Signature is valid"

rm selftest.bin selftest-signed.bin
```

If this doesn't print a valid-signature confirmation, stop -- do not proceed
to backup or upload. Re-generate from Step 1.

## Step 4 -- Back up the private key (before it ever leaves this machine)

Do this **before** Step 5. GitHub's encrypted secrets cannot be read back
once set (only overwritten), so this is the only point where the key's
plaintext bytes still exist -- if you skip the backup and something goes
wrong later, there is no way to recover it from GitHub.

Encrypt it (`age` shown; `gpg -c` is an equally reasonable substitute if
that's what you already use):

```bash
age -p -o midad-ota-signing-key-production.pem.age \
  midad-ota-signing-key-production.pem
# Choose a strong passphrase; store the passphrase itself in a *different*
# location than the encrypted file (e.g. your password manager for the
# passphrase, a separate physical location for the encrypted file).
```

Store `midad-ota-signing-key-production.pem.age` in **at least two**
independent, access-controlled locations -- for example:
- A password manager's secure-note file attachment (if your team already
  uses one for other release credentials -- `docs/ota-signing-key-management.md`
  recommends matching existing custody practice, not inventing a new one).
- An offline, encrypted USB drive kept in physical custody, not networked.

Never store the **unencrypted** `.pem` file anywhere persistent -- not in
cloud storage, not in a git repo (even private), not in an unencrypted
password-manager text field.

## Step 5 -- Provision the GitHub Actions secret

```bash
gh secret set OTA_SIGNING_KEY --repo sfoulad/midad-by-foulad \
  < midad-ota-signing-key-production.pem
```

Reading the file via stdin redirect (`<`), not as a command-line argument,
keeps the key out of shell history and `ps` output. `gh secret set` does not
echo the value back -- there is no "verify it was set correctly" read-back
step; verification instead happens naturally the first time a signing step
runs (Step 8 below) and either succeeds or fails closed exactly as
`sign_firmware.sh` already does today.

## Step 6 -- Commit the public key

```bash
cd /path/to/midad-by-foulad   # the actual repo, not the ceremony directory
cp ~/ota-signing-ceremony/ota-signing-public-key.pem ota-signing-public-key.pem
git checkout -b chore/provision-ota-signing-key
git add ota-signing-public-key.pem
git commit -m "chore: provision production OTA signing public key"
git push -u origin chore/provision-ota-signing-key
gh pr create --title "chore: provision production OTA signing public key" \
  --body "Commits the public half of the production OTA signing key (task #198). Private half provisioned as the OTA_SIGNING_KEY secret out of band -- see docs/ota-production-key-ceremony.md."
```

This is a **public** key -- safe to commit, safe to be visible in git
history forever. It is the *private* half that must never touch this repo,
any branch, any commit, or any CI log.

## Step 7 -- Clean up the ceremony machine

```bash
rm -f ~/ota-signing-ceremony/midad-ota-signing-key-production.pem
# macOS has no `shred`; if the drive isn't already FileVault-encrypted,
# consider that a gap to close independent of this ceremony, not something
# this step can fix after the fact.
history -d $(history 1) 2>/dev/null || true   # best-effort; see note below
```

Shell history hygiene is a **best-effort backstop**, not the actual control
-- Step 5 already avoided putting the key value in argv/history by using
stdin redirection, so there shouldn't be anything sensitive in history to
clean. Do verify: `history | grep -i "ota-signing-key-production"` should
show only file*path* references, never the key bytes themselves.

## Step 8 -- Confirm the secret works (first real signing attempt)

The `Auto Release Candidate` workflow already runs on every push to
`develop` and will pick this up automatically on the next qualifying push --
no separate trigger needed to confirm Step 5 worked. Watch the next run's
`Sign firmware` step: it should now succeed instead of failing closed with
`OTA_SIGNING_KEY is not set`.

If you'd rather confirm immediately without waiting for a qualifying push,
the multi-board RC workflow (Step 9 below) is the deliberate way to do that
-- don't manually re-run `auto-release.yml`'s existing failed run, since
`workflow_dispatch` isn't wired for it (see that workflow's own header
comment for why).

## Step 9 -- Produce and inspect the first signed RC (every board)

`release_candidate.yml` is the deliberate, manually-triggered, all-4-board RC
builder (distinct from `auto-release.yml`, which auto-triggers per push but
only builds the X3/X4 `gh_release_rc` board). It requires running from a
branch named `release/<version>` (matches the existing convention: see
`release/1.7.81`, `release/1.7.80` in this repo's branch history).

```bash
# From an up-to-date develop:
git checkout develop && git pull
VERSION=$(grep -m1 '^version = ' platformio.ini | cut -d' ' -f3)
git checkout -b "release/$VERSION"
git push -u origin "release/$VERSION"

gh workflow run release_candidate.yml --repo sfoulad/midad-by-foulad \
  --ref "release/$VERSION"
```

This builds and signs all 4 boards -- `gh_release_rc` (X3/X4),
`sticky-gh_release_rc`, `x4pro-gh_release_rc`, `papermono-gh_release_rc` --
and publishes them as a single **pre-release** (`prerelease: true` in
`release_candidate.yml`), tagged `v<version>-rc+<sha>`. This is a real
GitHub Release and a real git tag, but explicitly not a *stable* release --
matches "produce ... a signed RC ... do not publish a stable release yet."

### Cryptographic inspection (do this for every board's binary, before trusting the RC)

Download each signed artifact, then for each one:

```bash
# 1. Confirm the signature verifies against the committed public key
espsecure.py verify_signature --version 2 \
  --keyfile ota-signing-public-key.pem firmware<board>.bin
# Expect: "Signature is valid"

# 2. Inspect the signature block structure -- confirm exactly one block,
#    RSA-3072, and record the key digest for comparison across boards
#    (all 4 should show the identical digest -- same key signed all of them)
espsecure.py signature_info_v2 firmware<board>.bin

# 3. Record the SHA-256 of the signed artifact as the release's canonical
#    hash (cross-check against what the workflow run's own logs show)
shasum -a 256 firmware<board>.bin
```

Expect all 4 boards to report the **same** key digest in step 2 -- if any
board shows a different digest, something in that board's build/sign step
diverged and must be investigated before trusting that artifact.

### What this step does NOT do

- Does not flash any hardware (task #197/#137 -- blocked on a disposable
  test device, explicitly out of scope for this ceremony).
- Does not publish a stable release (`release.yml`, triggered by a version
  tag push, not by this workflow).
- The pre-release this creates is real and public the moment
  `release_candidate.yml` completes -- if the cryptographic inspection above
  finds a problem, delete the GitHub Release and tag
  (`gh release delete v<version>-rc+<sha> --repo sfoulad/midad-by-foulad --cleanup-tag`)
  before treating it as validated.

## Recovery-drill note (not part of this ceremony, tracked separately)

`docs/ota-signing-key-management.md`'s "Practical implication for Phase 1"
section recommends rehearsing "Rotation option 1" (the bridge-release
procedure) once, deliberately, with a throwaway key, before it's ever needed
under pressure. That rehearsal is a separate, later action item -- not a
prerequisite for provisioning the real production key, and not performed by
this document.
