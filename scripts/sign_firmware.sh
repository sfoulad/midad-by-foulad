#!/usr/bin/env bash
# Signs a release firmware.bin with espsecure.py (ESP-IDF's own Secure Boot V2
# RSA-3072 tooling -- see docs/ota-signing-key-management.md) and verifies the
# result before letting a release workflow publish it. Fails closed: any missing
# input (private key secret, public key file, signing/verification error) is a
# hard failure, not a silent skip -- see docs/ota-migration-architecture.md for
# why publishing even one unsigned "signed-enabled" release would OTA-lock every
# device that installs it.
#
# Usage: sign_firmware.sh <firmware.bin> <public-key.bin>
# Env:   OTA_SIGNING_KEY -- the private signing key, PEM format, as a single
#        environment variable value (GitHub Actions encrypted secret).

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <firmware.bin> <public-key.bin>" >&2
  exit 1
fi

FIRMWARE="$1"
PUBLIC_KEY="$2"

if [[ -z "${OTA_SIGNING_KEY:-}" ]]; then
  echo "::error::OTA_SIGNING_KEY is not set -- refusing to publish an unsigned release." >&2
  echo "See docs/ota-signing-key-management.md for how the production key is provisioned." >&2
  exit 1
fi

if [[ ! -f "$PUBLIC_KEY" ]]; then
  echo "::error::Public verification key '$PUBLIC_KEY' not found -- refusing to publish." >&2
  echo "This file must exist once a production key is provisioned (see docs/ota-signing-key-management.md)." >&2
  exit 1
fi

if [[ ! -f "$FIRMWARE" ]]; then
  echo "::error::Firmware artifact '$FIRMWARE' not found." >&2
  exit 1
fi

KEY_FILE="$(mktemp)"
trap 'rm -f "$KEY_FILE"' EXIT
printf '%s' "$OTA_SIGNING_KEY" > "$KEY_FILE"

SIGNED="${FIRMWARE%.bin}-signed.bin"
# espsecure.py (with the .py suffix) and underscored subcommands: this is real,
# PyPI-installable esptool (pinned to 4.12.0 in the workflow that calls this
# script), NOT PlatformIO's bundled tool-esptoolpy copy -- that copy reports a
# different internal version (5.1.2) that does not correspond to any published
# PyPI esptool release, has no `pip install`-able equivalent, and uses the
# newer hyphenated `espsecure`/`sign-data` command names. Confirmed by actually
# running both: same signature format, same accept/reject behavior, functionally
# interchangeable -- but the command names are not, and a `pip install esptool`
# step can only ever produce the PyPI one.
espsecure.py sign_data --version 2 --keyfile "$KEY_FILE" --output "$SIGNED" "$FIRMWARE"

# Verify against the checked-in public key before trusting our own signing step's
# exit code -- catches a wrong/corrupt key or a tooling regression before publish,
# not after a device rejects it in the field.
espsecure.py verify_signature --version 2 --keyfile "$PUBLIC_KEY" "$SIGNED"

mv "$SIGNED" "$FIRMWARE"
echo "Signed and verified: $FIRMWARE"
