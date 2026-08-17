# OTA Signing: Hardware Tamper-Test Checklist (Part E, not yet run)

Status: **prepared, not executed.** Requires a dedicated, disposable X3/X4 test
device -- the reference device recon'd during Milestone 2 (`38:44:be:e1:d8:40`) is
explicitly **not** approved for this (see the `phase1-ota-test-hardware` memory).
Nothing in this checklist has been run. No hardware has been flashed for these
tests.

## Prerequisites before starting
1. A confirmed disposable test device (explicit user sign-off on the specific
   MAC/port, matching the same identification discipline used for the reference
   device's recon).
2. Read-only recon first, same as the reference device: `flash_id`, partition
   table read-back, otadata parse, boot-log capture, full-flash backup +
   checksum -- confirms starting state and gives a restore path if anything
   goes wrong, before any write.
3. A throwaway dev signing keypair (never the production key -- none exists yet
   anyway). Reuse the pattern from `docs/ota-signing-key-management.md`'s
   "Development-only test keys" section: `espsecure.py generate_signing_key
   --version 2 --keyfile <path>`, excluded from git, never embedded as a real
   release's trust root.
4. A build of the current branch with `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`
   etc. already enabled (done, Milestone 1) -- flash this as the device's
   starting firmware for every test below, so the running app's own signature
   block (signed with the throwaway dev key) is the trust anchor being tested
   against.

## Open implementation question to resolve when Part E starts (not resolved here)
`OtaUpdater.cpp`'s release-check URLs
(`https://api.github.com/repos/sfoulad/midad-by-foulad/releases/...`) are
hardcoded `constexpr`, with no existing build-flag override to point a test
device at a local server. Testing the *real* HTTP OTA path end-to-end (most
faithful to production) needs one of:
- **(preferred)** a small, clearly-scoped, test-only URL override (e.g. gated
  behind a new `-D` flag, never defined in `default`/`gh_release*` envs) so the
  device's `checkForUpdate()`/`installUpdate()` hit a local test HTTP server
  serving the crafted images -- this is a real, if small, firmware change and
  should go through the same review as any other code change once Part E
  actually starts, not be added speculatively now.
- **(fallback, narrower)** since the signature check itself lives entirely
  inside `esp_ota_end()`/`esp_image_verify()` and doesn't care how bytes
  arrived in the staging partition, tests 2-5 below (tamper/wrong-key/
  unsigned/truncated) could instead write the crafted image directly to the
  inactive OTA partition via `esptool.py write_flash` at its known offset, then
  trigger the app-level `esp_ota_end()`/`esp_ota_set_boot_partition()`
  sequence through a temporary debug hook. This tests the verification logic
  itself faithfully but skips exercising `HttpDownloader`/`OtaUpdater`'s own
  code -- weaker coverage of the app-level integration, but no code change
  needed to reach it if the URL-override approach turns out to be unwanted.

Either way, decide and document the choice at the start of the actual Part E
session -- don't silently default to one without saying so in that session's
report.

## The seven tests

For each: capture full serial log (`docs/debugging_monitor.py` or raw serial
capture), note the exact `esp_err_t` string and log lines seen, and compare
against the "expected" column below -- sourced directly from the pinned
ESP-IDF, not guessed.

| # | Test | How to produce the artifact | Expected device behavior | Expected log lines (source-cited) |
|---|---|---|---|---|
| 1 | Valid signed image | `espsecure.py sign_data --version 2 --keyfile <devkey> --output firmware-signed.bin firmware.bin` (same dev key the running app already trusts) | Accepted, installs, reboots into new version | `[INF] [OTA] Update completed` (`OtaUpdater.cpp`); ESP-IDF side: `ESP_LOGI(TAG, "#%d app key digest == #%d trusted key digest", ...)` then `ESP_LOGI(TAG, "Verifying with RSA-PSS...")` (`secure_boot_signatures_app.c`), no `esp_ota_ops` error |
| 2 | Bit-flipped signed image | Sign normally, then flip one byte well inside the app body (not the trailing signature block) | Rejected, current firmware keeps running | `[ERR] [OTA] esp_ota_end failed: ESP_ERR_OTA_VALIDATE_FAILED` (`OtaUpdater.cpp`); underneath: `ESP_LOGE(TAG,"New image failed verification")` (`esp_ota_ops.c` TAG=`esp_ota_ops`). `esp_image_verify()` checks the image checksum *before* signature verification, so a bit flip can be caught there alone (`ESP_LOGE(TAG, "Checksum failed")`, TAG=`esp_image`) with **neither** signature-specific fallback log appearing -- do not treat their absence as a failed test. If the checksum still passes and signature verification runs, expect `ESP_LOGE(TAG, "Secure boot signature verification failed")` followed by `ESP_LOGW(TAG, "image valid, signature bad")` only if the image hash still matches (isolated corruption), or `ESP_LOGW(TAG, "image corrupted on flash")` if the hash no longer matches (`esp_image_format.c` TAG=`esp_image`) |
| 3 | Wrong-key-signed image | Sign with a *different* throwaway dev key than the one the running app trusts | Rejected, current firmware keeps running | Same top-level `ESP_ERR_OTA_VALIDATE_FAILED` as test 2, but `secure_boot_signatures_app.c` never finds a matching trusted digest at all -- no `"#%d app key digest == #%d trusted key digest"` match line appears; falls through the block loop with `any_trusted_key=false` |
| 4 | Unsigned image | Plain `firmware.bin`, never run through `sign_data` | Rejected once the device is in signed-only state | Same `ESP_ERR_OTA_VALIDATE_FAILED` top-level; `secure_boot_signatures_app.c`'s `validate_signature_block()` fails the magic-byte/CRC check on the (absent/garbage) trailing region -- no valid block found, loop finds nothing to compare |
| 5 | Truncated image | Sign normally, then truncate the file partway through (e.g. `head -c <n>`) | Rejected; also worth checking whether it's rejected earlier, at the *download* layer, since a real OTA fetch of a truncated artifact should already fail `HttpDownloader`'s completeness check before reaching `esp_ota_end()` at all | If reached via the direct-write fallback method (bypassing HTTP): checksum/structural failure inside `esp_image_verify()`'s earlier stages (`process_checksum`/`process_appended_hash_and_sig`), likely `ESP_ERR_IMAGE_INVALID` before signature checking is even reached. If reached via the real HTTP path: `esp_http_client_read()` returning a negative value produces `[ERR] read error after %zu bytes` (`FailStage::READ`) at any point mid-stream, while the stream ending short of the expected `Content-Length` produces `[ERR] [HTTP] incomplete: got %zu of %zu bytes` (`FailStage::INCOMPLETE`) -- accept **either** as the correct rejection, both are real `runGet()` outcomes for a truncated/severed transfer, not just `INCOMPLETE`. Either way: `[ERR] [OTA] Firmware install failed (download)` (`OtaUpdater.cpp`), `esp_ota_abort()` called, `esp_ota_end()` never reached at all (see Milestone 2's traced call sequence in `docs/ota-migration-architecture.md`) |
| 6 | Interrupted OTA (kill connection mid-transfer, real HTTP path only) | Start a real OTA download, sever the connection (stop the local test server, or disconnect WiFi) partway through | Current firmware must remain intact and running; no partial/corrupt boot | Same `FailStage::READ`-or-`FailStage::INCOMPLETE` / `esp_ota_abort()` path as test 5's HTTP variant -- require `esp_ota_abort()` and no boot-partition switch as the invariant regardless of which of the two stages fires. **Per the read-only recon already done**: normal OTA always targets the *inactive* slot (confirmed on the reference device: app0 active, app1 empty) -- an interruption writes only to the inactive partition and cannot corrupt the currently-running one by construction, so this should be safe by design; the test's purpose is confirming that in practice, on real hardware, not assuming it |
| 7 | Rollback/reboot behavior | After a rejected update (any of tests 2-5), and separately after forcing a crash-loop on a *freshly installed* signed image (if `CONFIG_APP_ROLLBACK_ENABLE` is being exercised) | Device continues booting the last-known-good, already-verified partition; document actual behavior precisely rather than assuming | Boot log should show the same running-app version as before the failed attempt; if testing rollback-after-crash-loop specifically, watch for ESP-IDF's own rollback log lines (`esp_ota_ops.c`'s app-rollback path, separate from the install-time verification path covered above) |

## What "OTA authentication COMPLETE" requires
Per the explicit instruction on this milestone: do not claim completion until
tests 1-7 have actually passed on real hardware with logs captured matching (or
knowingly and explicitly diverging from, with a stated reason) the expected
column above. Source-code tracing (Milestone 2's Part A/B) establishes the
mechanism is *designed* to behave this way; it is not itself proof it *does*,
on real silicon, under real timing/interruption conditions.
