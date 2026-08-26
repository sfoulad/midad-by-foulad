#!/usr/bin/env bash
# Interactive macOS wizard that automates the production OTA signing key
# ceremony documented in docs/ota-production-key-ceremony.md. Guides the
# release maintainer through every step of that runbook in plain language,
# without ever printing, logging, or writing the private key or backup
# passphrase anywhere they could leak.
#
# Usage:
#   ./scripts/ota-key-ceremony.sh                  # the real production ceremony
#   ./scripts/ota-key-ceremony.sh --rehearsal      # throwaway-key rehearsal run
#   ./scripts/ota-key-ceremony.sh --preproduction  # pre-production ceremony
#                                                   # (single iCloud backup instead
#                                                   # of dual USB drives -- see
#                                                   # "--preproduction mode" below)
#
# Safety properties this script maintains throughout:
#   - The private key and passphrase are only ever held in memory (a bash
#     variable) or in files under 0700-permission temp directories that are
#     deleted before exit, via a trap that fires on success, error, or
#     interrupt alike.
#   - The passphrase is read with `read -s` (no terminal echo) and is fed to
#     gpg via a file descriptor backed by process substitution -- never as a
#     command-line argument (visible in `ps`) and never written to disk.
#   - `gh secret set` reads the private key via stdin redirection, not an
#     argument, for the same reason.
#   - Every decrypt operation writes to an explicit output file (`-o`), never
#     to stdout, so a restore-test can never print key material to the
#     terminal.
#
# --rehearsal mode: generates a real throwaway key, real dual encrypted
# backups (onto detected external volumes, same as production), and a real
# restore-test -- proving the cryptographic and backup mechanism end-to-end.
# It uses a clearly-separate secret name (deleted immediately after a
# round-trip check) and a clearly-separate public-key filename, and it never
# touches the real OTA_SIGNING_KEY secret, the real ota-signing-public-key.pem
# path, or git/PR state. Recommended to run once before ever running the real
# ceremony, and worth re-running any time this script changes.
#
# --preproduction mode: for the current pre-production stage, where no
# production customer devices depend on the OTA signing key yet and affected
# test/RC hardware can be physically re-flashed if the key is lost. It skips
# the dual-USB requirement in favor of a single encrypted backup written to a
# user-selected folder in iCloud Drive (the private key itself is never
# written there or anywhere else unencrypted), but otherwise provisions the
# REAL OTA_SIGNING_KEY secret and the REAL ota-signing-public-key.pem, and
# additionally writes a tracked ota-signing-key-status.md declaring the key
# pre-production-only (CI / RC generation / hardware qualification), not
# authorization for a stable production release. Compose with --rehearsal
# (--preproduction --rehearsal) to validate this mechanism with a throwaway
# key before real use.

set -euo pipefail

REPO="sfoulad/midad-by-foulad"
REAL_SECRET_NAME="OTA_SIGNING_KEY"
REAL_PUBKEY_FILENAME="ota-signing-public-key.pem"
REAL_STATUS_FILENAME="ota-signing-key-status.md"
MIN_PASSPHRASE_LEN=12
ICLOUD_ROOT="$HOME/Library/Mobile Documents/com~apple~CloudDocs"

REHEARSAL=0
PREPRODUCTION=0
OUTPUT_DIR=""

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

print_help() {
  cat <<'EOF'
OTA signing key ceremony wizard.

  --rehearsal          Run the full mechanism with a throwaway key. Never
                        touches the real OTA_SIGNING_KEY secret or the real
                        ota-signing-public-key.pem. Recommended before the
                        first real run. Composes with --preproduction to
                        rehearse that mode specifically.
  --preproduction      Pre-production ceremony: single encrypted backup in
                        a user-selected iCloud Drive folder instead of two
                        USB drives. Still provisions the REAL OTA_SIGNING_KEY
                        secret and REAL ota-signing-public-key.pem, plus a
                        tracked ota-signing-key-status.md marking the key
                        CI/RC/hardware-qualification-only -- not
                        authorization for a stable production release. Use
                        only when no production customer devices depend on
                        this key yet.
  --output-dir <path>  Where to place the finished public-key file
                        (production/pre-production mode only). Defaults to
                        the current git repository's root, or the current
                        directory if not inside one.
  -h, --help            Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rehearsal) REHEARSAL=1; shift ;;
    --preproduction) PREPRODUCTION=1; shift ;;
    --output-dir) OUTPUT_DIR="${2:-}"; shift 2 ;;
    -h|--help) print_help; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; print_help; exit 1 ;;
  esac
done

if [[ "$REHEARSAL" -eq 1 ]]; then
  SECRET_NAME="OTA_SIGNING_KEY_CEREMONY_REHEARSAL"
  PUBKEY_FILENAME="ota-signing-public-key.rehearsal.pem"
  STATUS_FILENAME="ota-signing-key-status.rehearsal.md"
else
  SECRET_NAME="$REAL_SECRET_NAME"
  PUBKEY_FILENAME="$REAL_PUBKEY_FILENAME"
  STATUS_FILENAME="$REAL_STATUS_FILENAME"
fi

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

step() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
info() { printf '    %s\n' "$1"; }
ok()   { printf '    \033[32m✓ %s\033[0m\n' "$1"; }
warn() { printf '    \033[33m! %s\033[0m\n' "$1"; }
fail() { printf '\033[31mERROR: %s\033[0m\n' "$1" >&2; exit 1; }

confirm() {
  # $1: prompt. Returns 0 for yes.
  local reply
  read -r -p "    $1 [y/N] " reply
  [[ "$reply" =~ ^[Yy]$ ]]
}

CEREMONY_DIR=""
cleanup() {
  local rc=$?
  if [[ -n "$CEREMONY_DIR" && -d "$CEREMONY_DIR" ]]; then
    # Best-effort overwrite before unlink. On SSD/APFS this is not a
    # cryptographic erasure guarantee (copy-on-write and wear-leveling mean
    # the original blocks may still exist until TRIM reclaims them) -- the
    # real control is that this directory only ever existed briefly, under
    # 0700, and FileVault (if enabled) encrypts the underlying blocks at
    # rest. This overwrite pass is a defense-in-depth backstop, not the
    # primary guarantee.
    find "$CEREMONY_DIR" -type f -exec sh -c 'dd if=/dev/urandom of="$1" bs=1024 count=$(( ($(stat -f%z "$1") / 1024) + 1 )) conv=notrunc 2>/dev/null' _ {} \; 2>/dev/null || true
    rm -rf "$CEREMONY_DIR"
  fi
  unset PASSPHRASE 2>/dev/null || true
  if [[ $rc -ne 0 ]]; then
    printf '\n\033[31mCeremony aborted (exit %d). Temporary files removed.\033[0m\n' "$rc" >&2
  fi
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Step: prerequisites
# ---------------------------------------------------------------------------

check_prereqs() {
  step "Checking prerequisites"

  [[ "$(uname -s)" == "Darwin" ]] || fail "This wizard is macOS-only (uses diskutil for backup-drive detection)."
  ok "Running on macOS"

  command -v espsecure.py >/dev/null 2>&1 || fail "espsecure.py not found. Install with: pip install esptool==4.12.0"
  ok "espsecure.py found"

  command -v gpg >/dev/null 2>&1 || fail "gpg not found. Install with: brew install gnupg"
  ok "gpg found ($(gpg --version | head -1))"

  command -v openssl >/dev/null 2>&1 || fail "openssl not found (should be preinstalled on macOS)."
  ok "openssl found"

  command -v gh >/dev/null 2>&1 || fail "gh CLI not found. Install with: brew install gh"
  gh auth status >/dev/null 2>&1 || fail "gh CLI is not authenticated. Run: gh auth login"
  ok "gh CLI authenticated"

  local perms
  perms="$(gh api "repos/$REPO" --jq '.permissions.push' 2>/dev/null || echo false)"
  [[ "$perms" == "true" ]] || fail "gh CLI does not have push/write access to $REPO."
  ok "Write access to $REPO confirmed"

  if git -C "$(pwd)" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    warn "You're running this from inside a git working tree. The ceremony's temp files live outside it (a private mktemp directory), so this is safe, but avoid running it from inside a directory you plan to 'git add -A' blindly."
  fi
}

# ---------------------------------------------------------------------------
# Step: backup drive selection
# ---------------------------------------------------------------------------

# Populates the global arrays VOL_IDS / VOL_NAMES / VOL_MOUNTS / VOL_WHOLE
# with every currently-mounted non-internal volume. VOL_WHOLE is the
# underlying physical disk identifier (e.g. "disk4" for partition "disk4s1"),
# used to catch two selections that are really the same physical drive under
# different partitions/volumes.
#
# Outside --rehearsal, mounted disk images ("BusProtocol": "Disk Image" --
# e.g. a leftover .dmg someone forgot to eject) are excluded: picking one as
# a "backup drive" in a real ceremony would silently produce a backup that
# evaporates the moment it's unmounted, with no physical medium backing it
# at all. --rehearsal deliberately keeps them, since that's how this script
# tests itself without needing spare physical USB hardware.
list_external_volumes() {
  VOL_IDS=(); VOL_NAMES=(); VOL_MOUNTS=(); VOL_WHOLE=()
  local mp plist dev_id internal whole bus
  for mp in /Volumes/*; do
    [[ -d "$mp" ]] || continue
    plist="$(diskutil info -plist "$mp" 2>/dev/null)" || continue
    dev_id="$(printf '%s' "$plist" | plutil -extract DeviceIdentifier raw -o - - 2>/dev/null || true)"
    [[ -n "$dev_id" ]] || continue
    internal="$(printf '%s' "$plist" | plutil -extract Internal raw -o - - 2>/dev/null || echo true)"
    [[ "$internal" == "false" ]] || continue
    bus="$(printf '%s' "$plist" | plutil -extract BusProtocol raw -o - - 2>/dev/null || echo "")"
    if [[ "$REHEARSAL" -ne 1 && "$bus" == "Disk Image" ]]; then
      continue
    fi
    whole="$(printf '%s' "$plist" | plutil -extract ParentWholeDisk raw -o - - 2>/dev/null || echo "$dev_id")"
    VOL_IDS+=("$dev_id")
    VOL_NAMES+=("$(basename "$mp")")
    VOL_MOUNTS+=("$mp")
    VOL_WHOLE+=("$whole")
  done
}

select_backup_targets() {
  step "Selecting two backup drives"
  info "Both backups need to be on SEPARATE physical drives, so one drive"
  info "failing (lost, damaged, stolen) never costs you the only copy."

  list_external_volumes
  if [[ "${#VOL_IDS[@]}" -lt 2 ]]; then
    fail "Found ${#VOL_IDS[@]} external volume(s) mounted, need at least 2. Insert two separate USB drives (each formatted and mounted) and re-run."
  fi

  echo
  local i
  for i in "${!VOL_IDS[@]}"; do
    printf '    [%d] %s  (%s, mounted at %s)\n' "$((i+1))" "${VOL_NAMES[$i]}" "${VOL_IDS[$i]}" "${VOL_MOUNTS[$i]}"
  done
  echo

  local pick1 pick2
  read -r -p "    Backup drive #1 (number): " pick1
  read -r -p "    Backup drive #2 (number): " pick2
  [[ "$pick1" =~ ^[0-9]+$ && "$pick2" =~ ^[0-9]+$ ]] || fail "Enter the drive numbers shown above."
  [[ "$pick1" -ge 1 && "$pick1" -le "${#VOL_IDS[@]}" && "$pick2" -ge 1 && "$pick2" -le "${#VOL_IDS[@]}" ]] || fail "Number out of range."
  [[ "$pick1" != "$pick2" ]] || fail "Pick two DIFFERENT drives, not the same one twice."
  [[ "${VOL_IDS[$((pick1-1))]}" != "${VOL_IDS[$((pick2-1))]}" ]] || fail "Those two selections resolve to the same physical volume."
  [[ "${VOL_WHOLE[$((pick1-1))]}" != "${VOL_WHOLE[$((pick2-1))]}" ]] || fail "Those two selections are both on the same physical drive (${VOL_WHOLE[$((pick1-1))]}) -- just different partitions/volumes on it. Pick two genuinely separate drives."

  BACKUP_DIR_1="${VOL_MOUNTS[$((pick1-1))]}"
  BACKUP_DIR_2="${VOL_MOUNTS[$((pick2-1))]}"
  ok "Backup #1: ${VOL_NAMES[$((pick1-1))]}"
  ok "Backup #2: ${VOL_NAMES[$((pick2-1))]}"
}

# --preproduction only: a single backup folder inside the user's iCloud
# Drive, in place of two independent physical USB drives. Only the
# ENCRYPTED backup file is ever written under this path (gpg_encrypt writes
# directly to its destination argument, never via an intermediate plaintext
# file) -- the private key itself never touches iCloud unencrypted.
select_backup_folder() {
  step "Selecting the pre-production backup folder"
  info "Pre-production mode uses a single encrypted backup in iCloud Drive"
  info "instead of two independent physical USB drives. Only the ENCRYPTED"
  info "backup file is ever written here -- the private key itself is never"
  info "written to this folder, or anywhere else in iCloud, unencrypted."
  echo

  [[ -d "$ICLOUD_ROOT" ]] || fail "iCloud Drive isn't set up on this Mac (expected: $ICLOUD_ROOT). Enable iCloud Drive in System Settings, or run the full ceremony (two USB drives, no --preproduction) instead."

  local input resolved real_resolved real_root
  read -r -p "    Folder inside iCloud Drive for the backup (e.g. 'OTA-Signing-Backup'): " input
  [[ -n "$input" ]] || fail "A folder name is required."
  case "$input" in
    /*) fail "Enter a folder name relative to iCloud Drive, not an absolute path." ;;
  esac

  resolved="$ICLOUD_ROOT/$input"
  mkdir -p "$resolved" || fail "Could not create $resolved"

  # Resolve symlinks/".." to a real path and confirm it's genuinely still
  # inside iCloud Drive -- a folder name containing ".." must not escape it.
  real_resolved="$(cd "$resolved" && pwd -P)"
  real_root="$(cd "$ICLOUD_ROOT" && pwd -P)"
  case "$real_resolved" in
    "$real_root"|"$real_root"/*) ;;
    *) fail "That resolves outside iCloud Drive ($real_resolved) -- pick a folder name without '..' or symlinks that escape it." ;;
  esac

  BACKUP_FOLDER="$real_resolved"
  ok "Backup folder: $BACKUP_FOLDER"
  warn "iCloud sync happens in the background, not instantly -- confirm this folder shows a synced (not just local) status in Finder before relying on it as your backup."
}

# ---------------------------------------------------------------------------
# Step: passphrase
# ---------------------------------------------------------------------------

read_passphrase() {
  step "Backup encryption passphrase"
  info "This protects both encrypted backup copies. It is never written to"
  info "disk, never logged, and never displayed -- memorize it, or write it"
  info "down and store it physically separate from both backup drives."
  info "Do NOT store it in the same password manager as either backup file."
  echo

  local p1 p2
  while true; do
    read -r -s -p "    Enter passphrase: " p1; echo
    if [[ "${#p1}" -lt "$MIN_PASSPHRASE_LEN" ]]; then
      warn "Too short -- use at least $MIN_PASSPHRASE_LEN characters."
      continue
    fi
    read -r -s -p "    Confirm passphrase: " p2; echo
    if [[ "$p1" != "$p2" ]]; then
      warn "Those didn't match. Try again."
      continue
    fi
    break
  done
  PASSPHRASE="$p1"
  unset p1 p2
  ok "Passphrase set (not displayed, not stored)"
}

# gpg helpers: passphrase is fed via a file-descriptor backed by process
# substitution, never as an argument (which `ps` would show) and never
# through a file on disk.
gpg_encrypt() {
  local src="$1" dst="$2"
  # Refuse to overwrite an existing backup at this path -- a re-run (e.g. a
  # future key rotation using this same script) must never silently destroy
  # a prior backup that might still be someone's only remaining copy.
  [[ ! -e "$dst" ]] || fail "$dst already exists -- refusing to overwrite a possibly-still-needed backup. Move it aside first, or pick a different drive."
  exec 3< <(printf '%s' "$PASSPHRASE")
  gpg --batch --yes --pinentry-mode loopback --passphrase-fd 3 \
    --symmetric --cipher-algo AES256 -o "$dst" "$src"
  exec 3<&-
}

gpg_decrypt() {
  local src="$1" dst="$2"
  exec 3< <(printf '%s' "$PASSPHRASE")
  gpg --batch --yes --pinentry-mode loopback --passphrase-fd 3 \
    --decrypt -o "$dst" "$src" 2>/dev/null
  exec 3<&-
}

# ---------------------------------------------------------------------------
# Main flow
# ---------------------------------------------------------------------------

step "OTA signing key ceremony"
if [[ "$REHEARSAL" -eq 1 ]]; then
  if [[ "$PREPRODUCTION" -eq 1 ]]; then
    printf '    \033[33m*** REHEARSAL MODE (pre-production mechanism): throwaway key, no production changes ***\033[0m\n'
  else
    printf '    \033[33m*** REHEARSAL MODE: throwaway key, no production changes ***\033[0m\n'
  fi
elif [[ "$PREPRODUCTION" -eq 1 ]]; then
  printf '    This generates a \033[1mPRE-PRODUCTION\033[0m OTA signing key and\n'
  printf '    provisions it as the real %s secret.\n' "$REAL_SECRET_NAME"
  printf '    Authorized for CI, signed RC generation, and hardware\n'
  printf '    qualification ONLY -- NOT authorization to ship a stable\n'
  printf '    production release.\n'
  printf '    Custody: a single iCloud Drive folder backup, not the dual-\n'
  printf '    USB production custody model. Accepted because no production\n'
  printf '    customer devices depend on this key yet.\n'
  echo
  read -r -p "    Type the phrase to continue: I understand this provisions a pre-production key, not for stable release
    > " confirm_phrase
  [[ "$confirm_phrase" == "I understand this provisions a pre-production key, not for stable release" ]] || fail "Confirmation phrase did not match. Aborting -- nothing was created."
else
  printf '    This generates the REAL production OTA signing key,\n'
  printf '    provisions it as the real %s secret, and\n' "$REAL_SECRET_NAME"
  printf '    prepares the real %s for its PR.\n' "$REAL_PUBKEY_FILENAME"
  echo
  read -r -p "    Type the phrase to continue: I understand this generates the real production key
    > " confirm_phrase
  [[ "$confirm_phrase" == "I understand this generates the real production key" ]] || fail "Confirmation phrase did not match. Aborting -- nothing was created."
fi

check_prereqs
if [[ "$PREPRODUCTION" -eq 1 ]]; then
  select_backup_folder
else
  select_backup_targets
fi
read_passphrase

CEREMONY_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ota-ceremony.XXXXXX")"
chmod 700 "$CEREMONY_DIR"
PRIVATE_KEY="$CEREMONY_DIR/private-key.pem"
PUBLIC_KEY="$CEREMONY_DIR/$PUBKEY_FILENAME"

step "Generating the RSA-3072 signing key"
espsecure.py generate_signing_key --version 2 --scheme rsa3072 "$PRIVATE_KEY" >/dev/null
chmod 600 "$PRIVATE_KEY"
ok "Key generated (held only in a private temp directory, deleted at the end)"

if [[ "$REHEARSAL" -eq 1 ]]; then
  BACKUP_FILENAME="midad-ota-signing-key.rehearsal.gpg"
elif [[ "$PREPRODUCTION" -eq 1 ]]; then
  BACKUP_FILENAME="midad-ota-signing-key.preproduction.gpg"
else
  BACKUP_FILENAME="midad-ota-signing-key.gpg"
fi

step "Creating encrypted backup(s)"
if [[ "$PREPRODUCTION" -eq 1 ]]; then
  BACKUP_PATHS=("$BACKUP_FOLDER/$BACKUP_FILENAME")
  gpg_encrypt "$PRIVATE_KEY" "${BACKUP_PATHS[0]}"
  ok "Backup written to iCloud Drive folder"
else
  BACKUP_PATHS=("$BACKUP_DIR_1/$BACKUP_FILENAME" "$BACKUP_DIR_2/$BACKUP_FILENAME")
  gpg_encrypt "$PRIVATE_KEY" "${BACKUP_PATHS[0]}"
  ok "Backup written to drive #1"
  gpg_encrypt "$PRIVATE_KEY" "${BACKUP_PATHS[1]}"
  ok "Backup written to drive #2"
fi

step "Restore-testing the backup(s)"
ORIGINAL_HASH="$(shasum -a 256 "$PRIVATE_KEY" | awk '{print $1}')"
for n in "${!BACKUP_PATHS[@]}"; do
  idx=$((n+1))
  RESTORE_TEST="$CEREMONY_DIR/restore-test-$idx.pem"
  gpg_decrypt "${BACKUP_PATHS[$n]}" "$RESTORE_TEST"
  RESTORED_HASH="$(shasum -a 256 "$RESTORE_TEST" | awk '{print $1}')"
  rm -f "$RESTORE_TEST"
  [[ "$ORIGINAL_HASH" == "$RESTORED_HASH" ]] || fail "Restore test FAILED for backup #$idx -- decrypted backup does not match the original key. Do not proceed. Re-run the ceremony from the start."
  ok "Backup #$idx restore-tested -- decrypts to the exact original key"
done

step "Extracting the public key"
espsecure.py extract_public_key --version 2 --keyfile "$PRIVATE_KEY" "$PUBLIC_KEY" >/dev/null
ok "Public key extracted"

step "Self-verifying the key works for signing"
# The restore-test above only proves gpg's own encrypt/decrypt round-trips
# correctly -- it says nothing about whether the RSA key itself is actually
# usable for ESP-IDF Secure Boot V2 signing. This is that separate check,
# matching the manual runbook's own Step 3, before this key is trusted with
# anything real.
SELFTEST_DATA="$CEREMONY_DIR/selftest.bin"
SELFTEST_SIGNED="$CEREMONY_DIR/selftest-signed.bin"
head -c 65536 /dev/urandom > "$SELFTEST_DATA"
espsecure.py sign_data --version 2 --keyfile "$PRIVATE_KEY" \
  --output "$SELFTEST_SIGNED" "$SELFTEST_DATA" >/dev/null
espsecure.py verify_signature --version 2 --keyfile "$PUBLIC_KEY" "$SELFTEST_SIGNED" >/dev/null \
  || fail "Sign/verify self-test FAILED -- this key is not usable for signing. Do not proceed. Re-run the ceremony from the start."
rm -f "$SELFTEST_DATA" "$SELFTEST_SIGNED"
ok "Sign/verify self-test passed"

step "Provisioning the GitHub secret"
if [[ "$REHEARSAL" -eq 1 ]]; then
  info "Rehearsal: setting throwaway secret '$SECRET_NAME', then deleting it."
else
  # Never silently replace an already-live production key: every device
  # that has installed a release signed with the current key can only ever
  # accept the *same* key's signature (see docs/ota-signing-key-management.md's
  # "Key rotation design") -- overwriting it here without a deliberate
  # rotation decision would strand every one of those devices.
  EXISTING_SECRET_NAMES="$(gh secret list --repo "$REPO" --json name --jq '.[].name')" \
    || fail "Could not check whether $SECRET_NAME is already provisioned (gh secret list failed) -- refusing to proceed without that confirmation."
  if printf '%s\n' "$EXISTING_SECRET_NAMES" | grep -qxF "$SECRET_NAME"; then
    fail "$SECRET_NAME is already provisioned on $REPO. This ceremony is for first-time provisioning only -- overwriting an already-live production key silently would strand every device that trusts it. If this is a deliberate key rotation, follow docs/ota-signing-key-management.md's rotation procedure instead of re-running this wizard."
  fi
fi
gh secret set "$SECRET_NAME" --repo "$REPO" < "$PRIVATE_KEY"
ok "Secret '$SECRET_NAME' provisioned (value cannot be read back -- this is a GitHub platform guarantee)"
if [[ "$REHEARSAL" -eq 1 ]]; then
  gh secret delete "$SECRET_NAME" --repo "$REPO" >/dev/null
  ok "Rehearsal secret deleted -- no trace left on the repo"
fi

step "Leak check"
LEAK_FOUND=0

# Shell history must not contain the private key's own bytes (file *paths*
# are fine and expected).
if history 2>/dev/null | grep -qF "BEGIN RSA PRIVATE KEY" 2>/dev/null; then
  warn "Shell history appears to contain raw key material -- investigate before proceeding."
  LEAK_FOUND=1
else
  ok "Shell history: clean"
fi

# Clipboard must not contain key material.
if command -v pbpaste >/dev/null 2>&1 && pbpaste 2>/dev/null | grep -qF "PRIVATE KEY"; then
  warn "Clipboard currently contains what looks like key material -- clear it (Cmd+C something else)."
  LEAK_FOUND=1
else
  ok "Clipboard: clean"
fi

# No stray decrypted copies anywhere under the ceremony directory besides
# the private key itself (which is about to be deleted) and the public key.
STRAY="$(find "$CEREMONY_DIR" -type f ! -name "private-key.pem" ! -name "$PUBKEY_FILENAME" 2>/dev/null || true)"
if [[ -n "$STRAY" ]]; then
  warn "Unexpected leftover files in the ceremony directory: $STRAY"
  LEAK_FOUND=1
else
  ok "No stray temp files"
fi

# A live `ps` scan for the passphrase is deliberately NOT done here: by the
# time this step runs, every gpg/espsecure.py invocation has already exited,
# so there is nothing left to observe -- a scan at this point can only match
# unrelated things (its own grep invocation, or whatever shell originally
# invoked this script) and would be a source of false positives, not a real
# check. The actual guarantee is structural: every gpg call in this script
# passes the passphrase via `--passphrase-fd` fed from a process-substitution
# pipe (never a `-c`/CLI argument, and never a plain heredoc, which bash
# implements via a real temp file), verified by code review and by the
# restore-test above actually succeeding. If that structural guarantee is
# ever weakened, review the diff for any `$PASSPHRASE` appearing inside a
# quoted argument passed directly to a command (grep this file for
# `passphrase-fd` and confirm every call site still matches this pattern).

[[ "$LEAK_FOUND" -eq 0 ]] || fail "Leak check found a problem -- see warnings above. Nothing was cleaned up automatically; investigate first."

step "Cleaning up plaintext material"
# The private key file itself is removed by the cleanup() trap at exit
# (best-effort overwrite, then unlink). Nothing else to do here except
# hand the public key to its final destination before that trap runs.

if [[ "$REHEARSAL" -eq 1 ]]; then
  FINAL_PUBKEY_DIR="$CEREMONY_DIR/rehearsal-output"
  mkdir -p "$FINAL_PUBKEY_DIR"
else
  if [[ -n "$OUTPUT_DIR" ]]; then
    FINAL_PUBKEY_DIR="$OUTPUT_DIR"
  else
    FINAL_PUBKEY_DIR="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
  fi
fi
mkdir -p "$FINAL_PUBKEY_DIR"
if [[ "$REHEARSAL" -ne 1 && -e "$FINAL_PUBKEY_DIR/$PUBKEY_FILENAME" ]]; then
  fail "$FINAL_PUBKEY_DIR/$PUBKEY_FILENAME already exists. This ceremony is for first-time provisioning only -- if this is a deliberate key rotation, handle the existing file deliberately rather than letting this script overwrite it."
fi
cp "$PUBLIC_KEY" "$FINAL_PUBKEY_DIR/$PUBKEY_FILENAME"
FINGERPRINT="$(openssl pkey -pubin -in "$PUBLIC_KEY" -outform DER 2>/dev/null | openssl dgst -sha256 | awk '{print $NF}')"

if [[ "$PREPRODUCTION" -eq 1 ]]; then
  if [[ "$REHEARSAL" -ne 1 && -e "$FINAL_PUBKEY_DIR/$STATUS_FILENAME" ]]; then
    fail "$FINAL_PUBKEY_DIR/$STATUS_FILENAME already exists. Handle the existing status file deliberately rather than letting this script overwrite it."
  fi
  PROVISIONED_DATE="$(date -u +%Y-%m-%d)"
  cat > "$FINAL_PUBKEY_DIR/$STATUS_FILENAME" <<STATUSEOF
# OTA Signing Key Status: PRE-PRODUCTION

**Status:** pre-production / release-candidate key -- NOT the stable
production signing key.

**Authorized uses:** CI, signed release-candidate (RC) generation, and
hardware qualification testing only.

**NOT authorized for:** signing a stable production release shipped to
customer devices. Provisioning this key is not, by itself, a decision to
ship a stable release signed with it.

**Custody model:** a single locally-encrypted backup stored in a
user-selected iCloud Drive folder -- not the dual-independent-physical-drive
custody model used for the production key (see
docs/ota-production-key-ceremony.md). This reduced custody was accepted
because no production customer devices depend on this key yet; affected
test/RC hardware can be physically re-flashed if the key is lost.

**Before public production launch:** the project will decide whether to
retain this key with strengthened custody (the full dual-USB production
ceremony) or rotate to a newly-generated production key with proper custody
from the start. See docs/ota-signing-key-management.md's key rotation
procedure.

**Public key fingerprint (SHA-256 of DER-encoded key):** $FINGERPRINT

**Provisioned:** $PROVISIONED_DATE via \`scripts/ota-key-ceremony.sh --preproduction\`
STATUSEOF
  ok "Status file written: $STATUS_FILENAME"
fi

echo
printf '\033[1m\033[32m================================================================\033[0m\n'
if [[ "$REHEARSAL" -eq 1 ]]; then
  printf '\033[1m\033[32m  REHEARSAL COMPLETE -- mechanism verified, no production state changed\033[0m\n'
else
  printf '\033[1m\033[32m  CEREMONY COMPLETE\033[0m\n'
fi
printf '\033[1m\033[32m================================================================\033[0m\n'
if [[ "$PREPRODUCTION" -eq 1 && "$REHEARSAL" -ne 1 ]]; then
  echo
  printf '\033[1m\033[33m  *** PRE-PRODUCTION KEY -- CI / RC / hardware qualification only ***\033[0m\n'
  printf '\033[1m\033[33m  *** NOT authorization to ship a stable production release ***\033[0m\n'
fi
echo
printf '  Public key fingerprint (SHA-256 of the DER-encoded key):\n'
printf '    %s\n' "$FINGERPRINT"
echo
if [[ "$PREPRODUCTION" -eq 1 ]]; then
  printf '  Backups: %d of %d written and restore-verified (single iCloud-folder\n' "${#BACKUP_PATHS[@]}" "${#BACKUP_PATHS[@]}"
  printf '  backup -- pre-production custody, see %s).\n' "$STATUS_FILENAME"
else
  printf '  Backups: %d of %d written and restore-verified.\n' "${#BACKUP_PATHS[@]}" "${#BACKUP_PATHS[@]}"
fi
if [[ "$REHEARSAL" -eq 1 ]]; then
  printf '  Rehearsal public key: %s/%s (throwaway, not for commit)\n' "$FINAL_PUBKEY_DIR" "$PUBKEY_FILENAME"
  if [[ "$PREPRODUCTION" -eq 1 ]]; then
    printf '  Rehearsal status file: %s/%s (throwaway, not for commit)\n' "$FINAL_PUBKEY_DIR" "$STATUS_FILENAME"
  fi
  printf '  Rehearsal secret: provisioned and deleted, no trace left.\n'
else
  printf '  Public key ready at: %s/%s\n' "$FINAL_PUBKEY_DIR" "$PUBKEY_FILENAME"
  if [[ "$PREPRODUCTION" -eq 1 ]]; then
    printf '  Status file ready at: %s/%s\n' "$FINAL_PUBKEY_DIR" "$STATUS_FILENAME"
  fi
  echo
  printf '  Next step -- commit and open the PR yourself:\n'
  printf '    cd %s\n' "$FINAL_PUBKEY_DIR"
  if [[ "$PREPRODUCTION" -eq 1 ]]; then
    printf '    git checkout -b chore/provision-preproduction-ota-signing-key\n'
    printf '    git add %s %s\n' "$PUBKEY_FILENAME" "$STATUS_FILENAME"
    printf '    git commit -m "chore: provision pre-production OTA signing key (CI/RC/hardware-qualification only)"\n'
    printf '    git push -u origin chore/provision-preproduction-ota-signing-key\n'
    printf '    gh pr create --title "chore: provision pre-production OTA signing key" --body "Pre-production key: authorized for CI, RC generation, and hardware qualification only. NOT authorization for a stable production release. See ota-signing-key-status.md. Task #198."\n'
  else
    printf '    git checkout -b chore/provision-ota-signing-key\n'
    printf '    git add %s\n' "$PUBKEY_FILENAME"
    printf '    git commit -m "chore: provision production OTA signing public key"\n'
    printf '    git push -u origin chore/provision-ota-signing-key\n'
    printf '    gh pr create --title "chore: provision production OTA signing public key" --body "Task #198."\n'
  fi
fi
echo
