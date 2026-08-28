# OTA Signing (Retired)

**Current posture: Midad does not cryptographically authenticate firmware.**
Midad follows CrossPoint's standard firmware format and release/OTA update
architecture unchanged -- the same as running upstream CrossPoint directly.
No signing step, boot-time signature check, or key material exists in this
build.

## History

Between 2026-08 and 2026-08-28 this project built and briefly operated an
app-image-signature verification pipeline: a pre-production signing key, a
`sign_firmware.sh` step in the release workflows, a boot-time check
(`src/OtaSigningBootGuard.cpp`) gated by `OTA_SIGNING_BOOT_CHECK_ENABLED`,
and supporting key-ceremony/custody tooling and documentation
(`scripts/ota-key-ceremony.sh`, `docs/ota-production-key-ceremony.md`,
`docs/ota-migration-architecture.md`, `docs/ota-hardware-tamper-test-checklist.md`).

That pipeline verified signature-block *structure* but never authenticated
the signing key itself, and a production custody upgrade / key rotation was
never completed. It was retired in full -- see this repository's git history
around the commit that removed it for the exact scope of what existed and
why the decision was made.

## If cryptographic firmware authentication is revisited

Do not restore the retired implementation as-is. Any future signing
mechanism should be re-derived against CrossPoint's own upstream state at
that time (or adopted directly from an upstream CrossPoint mechanism, if one
exists by then) rather than resurrected from this project's prior spike --
the previous work never resolved the signing-key-authentication gap noted
above, and no production custody model was ever put into service.
