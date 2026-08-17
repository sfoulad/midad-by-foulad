# OTA Signing: Trust-Chain Mechanics and Existing-Device Migration

Status: design only, Phase 1 Milestone 2. No migration release has been built or
published. Companion to `docs/ota-signing-key-management.md` (key custody, signing
pipeline, rotation) -- this document covers *how the verification mechanism actually
behaves* and what that means for moving the existing unsigned fleet onto it.

## Confirmed architecture facts (source-verified against this project's pinned
ESP-IDF -- `~/.platformio/packages/framework-espidf`, matching the toolchain
confirmed in Milestone 1: pioarduino 55.03.37 / Arduino core 3.3.7 / ESP-IDF
5.5.2.260206)

1. **Under `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, the verification trust key is
   taken from the signature block of the CURRENTLY RUNNING application partition**,
   not a compiled-in constant. `get_secure_boot_key_digests()`
   (`secure_boot_signatures_app.c:164-187`) checks `esp_secure_boot_enabled()`; when
   false (hardware Secure Boot off, our configuration), it calls
   `esp_secure_boot_get_signature_blocks_for_running_app()`
   (`secure_boot_signatures_app.c:146-162`), which resolves the running partition via
   `esp_ota_get_running_partition()` and reads whatever signature block(s) are
   appended there, live, off flash, on every verification call. Nothing is cached in
   NVS or baked into the app's own compiled code as a separate trust anchor.

2. **`esp_ota_end()` verifies the NEW/staging image against that currently-running
   application's trust key.** `esp_ota_end()` (`esp_ota_ops.c:478`) calls
   `ota_verify_partition()` (`esp_ota_ops.c:450`), which runs `esp_image_verify()`
   against `ota_ops->partition.staging` -- the partition that was just written, not
   the one currently booted. The key used to check that staging partition's signature
   comes from the *running* (old) partition per fact 1. Two different partitions:
   the image being verified, and the source of the key that verifies it.

3. **Verification is OTA-install-time protection, not boot-time Secure Boot.**
   `SECURE_BOOT_CHECK_SIGNATURE` (`esp_image_format.c:35-47`) is gated on
   `CONFIG_SECURE_SIGNED_ON_UPDATE` outside `BOOTLOADER_BUILD` (the app/OTA context)
   and on `CONFIG_SECURE_SIGNED_ON_BOOT` inside it. `CONFIG_SECURE_SIGNED_ON_BOOT_
   NO_SECURE_BOOT` (the only way to get bootloader-side checking without hardware
   Secure Boot) `depends on ... SECURE_SIGNED_APPS_ECDSA_SCHEME`
   (`bootloader/Kconfig.projbuild`) -- our scheme is RSA, not ECDSA, so this option
   is structurally unavailable to us. **The bootloader never checks signatures on
   ordinary boot in this configuration; verification only happens once, at the
   moment a new image is about to be installed.**

4. **Therefore an unsigned transition build with signed-app verification enabled is
   a dangerous state: after booting it, future authenticated OTA cannot establish
   trust because the running partition has no valid signature block.** Once that
   image is running, `calculate_image_public_key_digests()`
   (`secure_boot_signatures_app.c:66-144`) finds zero valid signature blocks
   (`num_digests == 0`), returns `ESP_ERR_NOT_FOUND`, and every subsequent
   `esp_secure_boot_verify_sbv2_signature_block()` call fails with "Could not read
   secure boot digests!" -- no key exists to check anything against, so every future
   OTA attempt is rejected regardless of whether the new image is itself validly
   signed. This device is **OTA-locked until physical recovery** (SD card, USB, or
   web flasher -- not a bootloop, not bricked; the device keeps running the
   transition firmware normally, it simply cannot accept another network update
   without physical intervention).

5. **Midad's current SD-card firmware path bypasses `esp_ota_ops` and therefore does
   NOT inherit this OTA signature verification automatically.**
   `FirmwareFlasher.cpp` writes directly via `esp_partition_erase_range`/
   `esp_partition_write` and hand-rolls the `otadata` update -- it never calls
   `esp_image_verify()`. Enabling the Kconfig protects the network-OTA path only;
   the SD-card path (and `freeink-sdk/libs/hardware/RecoveryBoot/`'s combo-boot
   path, same bypass) needs its own, separate design decision -- see below.

## Design decision: which paths are authenticated, which are physical recovery

- **Network OTA = mandatory signed path.** Enforced automatically by
  `esp_ota_ops`/`esp_image_format.c` once `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_
  BOOT=y` is compiled into the running firmware (already done, Milestone 1).
- **SD-card / USB / web-flasher / combo-boot recovery = the trusted physical
  recovery path, deliberately left unauthenticated.** This is not an oversight --
  it is the necessary escape hatch. Fact 4 above establishes that an OTA-locked
  device's *only* way back is physical access. If the physical paths were *also*
  signature-enforced, an OTA-locked device with no valid key (e.g. a bad transition
  release, or -- see Part D -- a lost signing key) would have **no way back in at
  all**. Physical-access attacks are already an accepted, documented risk for Phase
  1 (see `phase-1-plan.md` §14) -- someone with physical possession of the device
  can already open it, remove the SD card, and do far more than reflash firmware.
  Extending verification to the physical paths adds no meaningful protection against
  that threat model while removing the one recovery mechanism the signed-OTA design
  depends on. **This is an explicit, considered decision, not a silent gap**:
  physical paths stay open by design; only the remote/network path is authenticated.
  A future phase could revisit adding *optional* signature checking to the SD path
  with its own separate unsigned-recovery override (e.g. a boot-combo bypass), but
  that is new complexity correctly out of scope here.

## Migration path

### The core design simplification
Because fact 3 establishes that verification only ever happens at *install* time,
not at every boot, and because today's shipped firmware doesn't compile the
verification code path at all (`CONFIG_SECURE_SIGNED_ON_UPDATE` unset →
`SECURE_BOOT_CHECK_SIGNATURE` is `0` at compile time on every device in the field
today), **there is no need for a single, specific "transition version" every device
must pass through.** Old firmware's OTA-install logic never inspects the incoming
image for a signature block at all -- the preprocessor branch that would do so is
compiled out entirely. This means: *any* device, on *any* currently-shipped
version, installs *any* future signed release exactly the way it installs an
unsigned one today. "The transition release" is simply **the first release built
with signing enabled and correctly signed** -- not a single mandatory hop every
device must specifically land on before going further.

### Why the old→new hop is safe (traced through the actual code, not assumed)
1. **Byte-for-byte streaming, format-agnostic.** `OtaUpdater::installUpdate()`
   (`src/network/OtaUpdater.cpp`) never parses the ESP image format itself beyond
   reading the first 14 bytes for the chip-ID guard; it streams every received byte
   straight into `esp_ota_write()`. A signature block appended after the app image
   is just more opaque bytes to this code -- it gets written faithfully regardless
   of whether the currently-running (old) firmware understands what a signature
   block is.
2. **Old firmware's own verify pass tolerates the extra trailing bytes.**
   `process_appended_hash_and_sig()` (`esp_image_format.c`) computes
   `sig_block_len` only `#if CONFIG_SECURE_BOOT || CONFIG_SECURE_SIGNED_APPS_NO_
   SECURE_BOOT` -- on old firmware (neither set) this stays `0`, so its own
   understanding of `full_image_len` excludes the signature block. The only check
   applied is `full_image_len > part_len` (does the image fit in the partition) --
   a signature block trailing beyond what old firmware accounts for doesn't fail
   this check, it's simply extra data old firmware never reads or validates. Old
   firmware's actual content check (`verify_simple_hash`, run because
   `SECURE_BOOT_CHECK_SIGNATURE == 0`) validates only the plain SHA-256 hash
   appended directly after the app image, which sits *before* the signature block
   and is unaffected by whether one follows it.
3. **Partition headroom.** OTA partitions are 6.25MB each (`partitions.csv`); actual
   images are ~5MB; a signature block adds a fixed ~4096 bytes plus up to 4095 bytes
   of alignment padding (measured in Milestone 1: +6,800 bytes total). No truncation
   risk from partition sizing.
4. **A genuinely interrupted download is already caught before this matters.**
   `HttpDownloader::fetchUrlVerified()`'s underlying `runGet()` tracks
   `esp_http_client_is_complete_data_received()` against the declared
   Content-Length and returns `HTTP_ERROR`/`FailStage::INCOMPLETE` on a short
   transfer. `OtaUpdater::installUpdate()` checks `!fetchOk` and calls
   `esp_ota_abort(otaHandle)` *before* ever calling `esp_ota_end()`
   (`OtaUpdater.cpp:272-276`) -- a network interruption, even one that happens to
   land exactly within the trailing signature-block bytes, never reaches the
   partition-boot-switch step. This was verified by reading the exact call sequence
   in `installUpdate()`, not assumed.

**What this does *not* yet prove, and is explicitly deferred to Part E (hardware)**:
that a real, `espsecure.py`-signed image actually boots correctly on real X3/X4/
Sticky silicon, and that `esp_ota_end()` really does reject a tampered/wrong-key/
unsigned *next* image once a signed image is the one running. Those are runtime
behaviors on physical hardware; source-tracing establishes the mechanism is
*designed* to behave this way, but the Milestone 1 spike already showed one gap
between "Kconfig accepted" and "runtime-confirmed" (the auto-signing assumption)
that only got caught by actually running the tools -- so this document does not
claim hardware-level proof it doesn't have.

### Migration architecture (adopting Option A from your message, confirmed workable)

```
CURRENT UNSIGNED MIDAD  (any version, any age -- see below)
        |
        | ordinary OTA check + download, exactly as today.
        | Old firmware does not parse or care about the signature block; it just
        | streams bytes and applies its existing (non-signature) integrity checks.
        v
FIRST SIGNED RELEASE  ("the transition release" -- whichever version first ships
        |              with CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y AND a
        |              real espsecure.py-signed artifact)
        |
        | reboot. This release's own appended signature block is now read live off
        | flash as the trust anchor for every future verification (fact 1).
        v
SIGNED-ONLY MIDAD  -- every subsequent OTA is verified against that key. Unsigned or
        |             wrong-key images are rejected (Milestone 1's tamper-test
        v             matrix, to be re-confirmed on hardware in Part E).
   ALL FUTURE OTA MUST VERIFY
```

**The one hard requirement this design imposes, stated plainly**: every release
from the first signed one onward *must* actually be signed by the real production
key before being published as an OTA artifact -- not merely built with the Kconfig
on. Per fact 4, publishing even one unsigned "signed-enabled" release would
OTA-lock every device that installs it. This is why Part C's "CI fails closed if
signing is unavailable" requirement is not a nice-to-have; it is the single control
standing between a successful migration and a fleet-wide OTA-lock incident. Recommend
treating it as a release-blocking CI gate: the release workflow must refuse to
attach `firmware.bin` to a GitHub Release unless the signing step ran and produced a
verifiably signed artifact (e.g. by having CI itself run `espsecure.py
verify-signature` against the just-signed artifact and the checked-in public key
before uploading -- cheap, and closes the loop without trusting the signing step's
exit code alone).

### Scenario coverage

- **Users skipping "the" transition version**: not a special case. Any device
  jumping directly from an old version to a later signed release behaves exactly
  like the old→first-signed hop above -- old firmware still doesn't care about
  signature blocks regardless of how many versions it's skipping.
- **Very old Midad versions**: same reasoning: the OTA-install code path being
  exercised (old firmware, signature-unaware) is identical across all pre-signing
  versions. The only real constraint is the pre-existing, unrelated one of OTA
  protocol/format compatibility across large version gaps -- not new to this
  migration.
- **Rollback to an old unsigned OTA slot**: ESP-IDF's rollback (`CONFIG_APP_ROLLBACK_
  ENABLE=y`, crash-loop protection) just repoints the boot partition to the other
  OTA slot and reboots -- it does not invoke `esp_ota_ops`'s install-time
  verification (that only runs when *installing* a new image, not when booting an
  already-installed one). Rolling back to an old, unsigned-checking slot is
  uneventful: the device just runs that old firmware again, with no OTA
  verification until it once again installs and boots into a signed release.
  **This is a known, currently-open gap, not a resolved one**: the running app after
  such a rollback has signature verification compiled out, so its *next* OTA install
  can accept an unsigned or wrong-key image -- the exact pre-migration trust model,
  reopened. No policy decision has been made yet on how to handle it (options include:
  block rollback to a signature-unaware slot entirely, flag the device for mandatory
  physical recovery instead of an automatic rollback, or explicitly accept this as a
  bounded exception and test it). Treat this as unresolved until one of those is
  chosen and documented here, not as already covered by the "uneventful" framing
  above.
- **Interrupted transition (power/connectivity loss mid-download)**: already
  handled by existing code, independent of signing -- see point 4 above.
- **SD-card / USB / web-flasher / factory recovery**: deliberately unauthenticated,
  by design -- see "Design decision" above. This is also the answer for "signing-key
  loss or compromise" at the device level: an OTA-locked device (wrong/no key) is
  recovered the same way as any other -- physical reflash. Key-level rotation/
  recovery *strategy* (how to avoid needing this at fleet scale) is Part D, in
  `docs/ota-signing-key-management.md`.
