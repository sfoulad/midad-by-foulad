# OTA Signing: Key Management

Status: the CI/release integration design below is implemented -- `scripts/sign_firmware.sh`
and its call sites in `release.yml`/`release_candidate.yml`/`auto-release.yml` are live and
fail closed (see the "CI / release integration" section). **No production key exists yet**
(that provisioning step is owner-gated, not a Phase 1 software task), so every real release
workflow run currently refuses to publish -- correct, expected behavior until the key exists,
not a bug. The key-rotation design below is settled, corrected from Milestone 1's original
(wrong) assumption during Milestone 2 -- see that section for the source-verified reasoning.

## Scheme

RSA-3072, ESP-IDF Secure Boot V2's app-signing format
(`CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME`), used without hardware Secure Boot
(`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`). See
`phase-1-plan.md` §4-§6 for why this scheme and why not hardware Secure Boot for
Phase 1. Confirmed feasible in Midad's actual Arduino/PlatformIO toolchain via the
Milestone 1-B spike (`docs/../` -- spike results live in this session's scratchpad,
not the repo, since they're throwaway-key experiment output, not a design decision).

RSA-3072 is the only scheme `SOC_SECURE_BOOT_V2_RSA` supports on both ESP32-C3 and
ESP32-S3 -- one key format, one signing command, works for X3/X4/Sticky identically.

## Where the public verification key lives

Compiled into the firmware itself, as a `static const` byte array in a Midad-owned
source file (not a CrossPoint-owned one -- see `CLAUDE.md`'s thin-fork rule), embedded
via `extract-public-key`'s raw binary output. This is the trust anchor `esp_ota_ops.c`'s
`esp_image_verify()` checks OTA-installed images against (see `CLAUDE.md`'s Kconfig
Secure Boot notes: trust is anchored in the running app, not eFuse, since Secure Boot
hardware is off).

Flash-resident (`static const`), not a runtime-loaded value -- this is exactly the kind
of compile-time lookup table CLAUDE.md's Flash Persistence rule calls for, and there is
no scenario where this key should ever change without a full firmware rebuild anyway.

## Release-signing command

```
espsecure.py sign-data --version 2 --keyfile <private-key.pem> \
  -o firmware-signed.bin firmware.bin
```

Confirmed working against this project's actual `firmware.bin` output during the
Milestone 1-B spike (adds a fixed 4096-byte signature block plus up-to-4095 bytes of
sector-alignment padding). Run as an explicit step *after* PlatformIO's own build
completes -- confirmed during the spike that PlatformIO's SCons/esptool pipeline does
not auto-invoke ESP-IDF's `idf.py`-only CMake signing target, so this cannot be wired
up as a Kconfig-only change.

## CI / release integration (implemented)

- `scripts/sign_firmware.sh` signs and verifies the artifact; a "Sign firmware" step
  calls it in each of `release.yml`, `release_candidate.yml`, and `auto-release.yml`,
  after `pio run` (`gh_release`/`gh_release_rc` envs) and before the artifact is
  attached to the GitHub Release/pre-release.
- The private key is `secrets.OTA_SIGNING_KEY`, a GitHub Actions encrypted secret,
  injected only as an env var into that one step, written only to a `mktemp` file
  cleaned up via `trap ... EXIT`, never logged.
- Fails closed, confirmed in `sign_firmware.sh` itself: missing `OTA_SIGNING_KEY`,
  missing public key file, or missing firmware artifact are each a hard `exit 1`
  with an explanatory `::error::` before any publish step runs. A fork PR run
  (no repo secrets) or a run before the production key is provisioned both refuse
  to publish rather than shipping an unsigned artifact under a real release tag --
  the second case is exactly today's state, and is correct, not a bug.
- CI (`ci.yml`, PR builds) never touches the private key -- ordinary PR/branch
  builds stay unsigned, matching `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`'s
  default in `platformio.ini`. Only the three release-publish workflows sign.

## Private-key storage expectation

The production private key must never exist in this repository, in any branch, in any
commit, in any CI log, or on a developer's local disk outside of a deliberate,
access-controlled secret store. Recommended custody: a secrets manager the release
maintainer already controls access to (matching whatever the project already uses for
other release credentials), with GitHub Actions' encrypted-secrets mechanism as the
CI-side delivery method. A hardware security module or signing service is a reasonable
future upgrade but not a Phase 1 requirement -- the immediate goal is "not committed to
git and not sitting in plaintext on a laptop," not HSM-grade custody.

## Key rotation design (revised -- corrects a wrong Milestone 1 assumption)

**Milestone 1's version of this section was wrong** and is superseded by this one.
It assumed that because the RSA-3072 V2 signature *format* supports up to 3
independent signature blocks per image, dual-signing a release (current key in
block 0, new key appended in block 1) would let devices trusting either key accept
it -- standard practice under real hardware Secure Boot, where eFuse can hold and
independently revoke up to 3 trusted digests. **This does not hold under
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`.**

### What Milestone 2 verified (source: `secure_boot_signatures_app.c`, both by
reading the code and by an empirical dual-sign test with two throwaway keys)

`esp_secure_boot_verify_sbv2_signature_block()` (`secure_boot_signatures_app.c:226-296`)
declares:
```c
#ifdef CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
    const unsigned secure_boot_num_blocks = 1;
#else
    const unsigned secure_boot_num_blocks = SECURE_BOOT_NUM_BLOCKS;  // up to 3
#endif
```
This constant bounds *both* loops: which signature block of the **incoming** image
gets examined (`app_blk_idx < secure_boot_num_blocks`), and which trusted digest of
the **running app** gets compared against (`trusted_key_idx < secure_boot_num_blocks`).
Under our Kconfig, both are hard-limited to `1` -- **only block 0 on either side is
ever consulted.** `calculate_image_public_key_digests()` (the function that reads the
running app's own trusted digests) does structurally scan all 3 possible blocks and
would happily populate `trusted.key_digests[1]`/`[2]` if present -- but the comparison
loop that actually matters never reaches past index 0.

Confirmed empirically: signed `firmware-signed-A.bin` with key A, then ran
`espsecure.py sign-data --append-signatures --keyfile devkey-B-wrong.pem` on it.
`signature-info-v2` confirmed the result has two valid blocks -- key A's digest at
block 0 (unchanged), key B's digest at block 1. Nothing about that structure changes
which key ends up read into `trusted.key_digests[0]` after this file becomes a
running app: still whatever produced block 0.

**Conclusion: appending additional signature blocks has no effect at all under this
Kconfig combination.** There is no "either of N keys" trust model available here --
only whichever key occupies block 0 of the currently-running app matters, ever. This
is a real, if severe, simplification the "no Secure Boot hardware" trade-off carries:
multi-key support fundamentally depends on eFuse-backed independent key-slot
revocation, which doesn't exist when trust is instead read live from the running
app's own flash content.

### What this means: no OTA-only key rotation exists

A key, once it occupies block 0 of a device's running app, is the trust anchor for
that device until something with *physical* access changes it. There is no way,
using signing alone, to make a fleet's next-accepted release carry a different
block-0 key than the one already trusted -- the incoming image's block 0 must match
the current trust anchor to be accepted at all, by construction.

### Rotation option 1 (planned, remote, but reopens a real window -- the practical
default)

A working two-hop bridge, but with a cost that must be stated plainly, not glossed
over:
1. Ship a release signed with the **current** key (accepted normally), but built
   with `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` **disabled**. The
   OTA-install-time check that would reject an unsigned/wrong-key next image lives
   in the *currently running* app's compiled code, not the incoming image's -- so a
   validly-old-key-signed "verification-disabled" bridge is accepted exactly like
   any other current-key-signed release.
2. Once that bridge release is running on a device, it behaves exactly like today's
   pre-Phase-1 firmware: it accepts **any** next image, signed or not, with no
   signature check at all (`SECURE_BOOT_CHECK_SIGNATURE` compiled out again, same
   mechanism that lets any existing unsigned device accept the *original* transition
   release, see `docs/ota-migration-architecture.md`).
3. Ship a release signed with the **new** key, verification re-enabled. Once
   installed, its own block 0 (the new key) becomes the trust anchor going forward.
4. **The cost**: every device sits, for however long it takes to receive step 3
   after step 2, in the same unverified state Phase 1 exists to close -- a network
   attacker who catches a device in that window can push an arbitrary image, exactly
   as they can against today's shipped firmware. This is not a theoretical footnote;
   it is a deliberate, temporary reopening of the release-blocker vulnerability,
   traded off against not needing physical access to every device. Minimize the
   window (push step 3 as soon as adoption of step 2 is confirmed via
   `FouladDeviceTracking`), and treat this as an emergency-grade operation, not a
   routine one.

### Rotation option 2 (physical, no window, impractical at fleet scale)
Re-flash each device via SD card / USB / web flasher with an image signed by the new
key. No unverified window, since the physical path was never signature-gated to
begin with (see `docs/ota-migration-architecture.md`'s "Design decision" section) --
but requires physical access to every device, which is the whole reason a
remote-rotation option is wanted in the first place.

### Compromised key (attacker also has a copy, but we still have ours)
Use Option 1 immediately -- sign the bridge-and-new-key releases with the
compromised key one more time (a race against whoever else holds it, same race any
rotation design would face), and treat the exposure window as already-live urgency,
not routine timing.

### Lost key (nobody, including us, has it anymore)
**No remote recovery exists, under any design.** Devices that have already
established the lost key as their trust anchor can never again accept a
verification-passing OTA update -- Option 1 requires signing with the *current* key,
which is exactly what's gone. The only recovery is Option 2, physical re-flash of
every affected device. This is the single most important reason private-key custody
(above) cannot be treated casually: unlike a typical credential leak, losing this key
outright is not a "rotate and move on" incident -- it is closer to "every device in
the field needs a truck roll."

### Practical implication for Phase 1
Given no graceful rotation exists, key custody is the primary control, not a rehearsed
rotation drill. Still worth rehearsing Option 1 once, deliberately, before it's ever
needed under pressure (tracked as a later-milestone action item) -- but the design
goal should be "this key never needs rotating," not "rotation is cheap so custody can
be looser."

## Compromised-key emergency process (draft, needs review before Milestone 4)

1. Stop signing new releases with the compromised key immediately.
2. If a replacement key was already staged per the rotation design above, ship a
   dual-signed release with the new key added and the compromised key's trust
   *retained* only long enough for the fleet to update past it, then dropped.
3. If no replacement key was staged (worse case), the fleet has no path to a
   remotely-verified update until a new key reaches devices some other way -- this is
   the known, accepted limit of anchoring trust in the running app instead of eFuse
   (see `phase-1-plan.md` §14 Risks). There is no retroactive fix for this scenario;
   the mitigation is rehearsing rotation *before* it's needed, not recovering after.
4. Any suspected compromise gets the same incident-response treatment as any other
   credential leak: rotate, audit how it leaked (CI logs, a developer machine, a
   third-party service), and only resume normal release signing after the root cause
   is closed.

## Development-only test keys

The two keys used in the Milestone 1-B spike (`devkey-A.pem`, `devkey-B-wrong.pem`)
were generated with `espsecure.py generate-signing-key --version 2 --scheme rsa3072`,
used only inside this session's gitignored scratchpad directory, and were never
committed, embedded in firmware, or used to sign a real release artifact. Any future
disposable development key must follow the same rule: excluded from git (add to
`.gitignore` if a conventional path emerges, e.g. `*.signing-key.pem`), clearly
distinguishable from the production key by filename, and never allowed to become the
public key baked into a real release build. A dev key that's mistakenly built into a
release is a full compromise of that release's trust anchor -- it defeats the entire
point.
