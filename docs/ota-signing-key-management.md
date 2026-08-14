# OTA Signing: Key Management

Status: design only, Phase 1 Milestone 1. No production key exists yet. Nothing in
this document has been implemented in the release pipeline -- that is Milestone 4
("OTA signing implementation") per `phase-1-plan.md`'s milestone sequence. This
document exists so that when Milestone 4 starts, the key-handling decisions are
already made and reviewed, not improvised under release pressure.

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

## CI / release integration design (not yet implemented)

- Add a signing step to whichever GitHub Actions workflow produces the public release
  artifact (`gh_release`/`gh_release_rc` envs) -- after `pio run`, before the artifact is
  attached to the GitHub Release.
- The private key must be a GitHub Actions encrypted secret, injected only into that
  one step, never written to a file the rest of the job can read, never logged.
- The workflow should fail closed: if the secret is unavailable (e.g. a fork PR run,
  which doesn't get repo secrets), the job must fail rather than publish an unsigned
  release artifact under a real release tag.
- CI (`ci.yml`, PR builds) never needs the private key -- ordinary PR/branch builds
  stay unsigned, matching `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`'s default in
  `platformio.ini`. Only the actual release-publish job signs.

## Private-key storage expectation

The production private key must never exist in this repository, in any branch, in any
commit, in any CI log, or on a developer's local disk outside of a deliberate,
access-controlled secret store. Recommended custody: a secrets manager the release
maintainer already controls access to (matching whatever the project already uses for
other release credentials), with GitHub Actions' encrypted-secrets mechanism as the
CI-side delivery method. A hardware security module or signing service is a reasonable
future upgrade but not a Phase 1 requirement -- the immediate goal is "not committed to
git and not sitting in plaintext on a laptop," not HSM-grade custody.

## Key rotation design

RSA-3072 V2 signature blocks support up to 3 independent signature blocks per image
(confirmed during the spike -- `espsecure.py`'s own output enumerates "Signature block
0/1/2" slots). Rotation plan:
1. Generate the new key.
2. Sign one release with *both* the current and new key (multi-signature `sign-data`
   invocation, or repeated `sign-data --append-signatures`).
3. Ship that dual-signed release; devices already on the old key still verify
   (old signature block still present and valid), devices that will trust the new key
   going forward now have both.
4. Once fleet adoption of the dual-signed release (or later) is confirmed via
   `FouladDeviceTracking` check-ins, retire the old key from future signing.

This should be rehearsed once, deliberately, before it is ever needed under pressure --
tracked as a Milestone-4-or-later action item, not resolved by this document alone.

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
