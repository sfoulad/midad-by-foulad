#!/usr/bin/env bash
#
# update-survivability-gate.sh
#
# Fails a pull request that touches the firmware update path -- the flasher,
# the OTA client, the boot-slot switch, the image validators, the partition
# table, or the release/signing machinery -- unless it carries a hardware
# validation report proving the device can still install a SECOND update after
# the change.
#
# Why this exists
# ---------------
# A UI/touch hardware-test RC also changed the firmware installation path and
# shipped without a two-slot update test. It installed once; the device could
# then never install again, because runningPartitionChipId() (see
# src/network/FirmwareFlasher.cpp) misreads chip identity from the running
# slot, and BOTH install paths fail closed on that value:
# src/network/FirmwareFlasher.cpp's validateImageFile() returns BAD_CHIP, and
# src/network/OtaUpdater.cpp's installUpdate() aborts the transfer. The only
# X4 Pro in the field became update-locked with no working recovery channel.
#
# Four board builds, 512 host tests, cppcheck, CodeQL and a full CodeRabbit
# review all passed. None of them runs on hardware, and none of them exercises
# an install CYCLE. This gate is the check that would have caught it, and the
# only evidence it accepts is a real device completing the matrix in
# docs/update-survivability-gate.md.
#
# Classification is DIFF-DERIVED, not metadata-derived
# ----------------------------------------------------
# The gate triggers on the changed-file list alone. A PR label, a title
# convention, or a "this is only UI work" claim in the description cannot turn
# it off -- there is nothing to remove, mislabel, or forget. Label and title
# signals are read (see ui_presentation below), but they can only ESCALATE the
# message, never suppress the gate. See the workflow file's header for the
# residual failure mode this choice leaves open.
#
# Fail-closed contract
# --------------------
# Exit 0: no protected path touched, OR a complete hardware validation report
#         is present and every matrix row passed.
# Exit 1: a protected path was touched and the report is missing, incomplete,
#         or records a FAIL/BLOCKED row.
# Exit 2: the gate could not DETERMINE the answer -- an unreadable or empty
#         changed-file list, an unreadable evidence file. The caller MUST
#         treat exit 2 as a failure, never as a pass. Reading a missing input
#         as "nothing to check" is the identical fail-open mistake that
#         produced the incident this gate exists to prevent.
#
# Renames are a first-class hazard, not a detail
# ----------------------------------------------
# Classifying only the DESTINATION path of a rename is a complete bypass of
# this gate: a PR that renames src/network/OtaUpdater.cpp to
# src/network/Updater.cpp AND rewrites it in the same commit presents a
# changed-file list that matches no protected pattern at all, so the gate
# reports "Not applicable" and waves the updater change through with no
# hardware validation. Both sides of every rename are therefore classified
# independently -- the same conclusion scripts/thin-fork-guard.sh reached for
# its security-boundary files, and for the same reason.
#
# Usage:
#   update-survivability-gate.sh CHANGED_FILES EVIDENCE LABELS PR_TITLE
#
#   CHANGED_FILES  file, one CHANGED RECORD per line. A record is either a
#                  bare repo-relative path, or a rename written as
#                  `destination<TAB>source` -- the field order of the GitHub
#                  pull-files API, which reports a rename's destination in
#                  `filename` and its source in `previous_filename`. A line
#                  with no tab (or an empty second field) is a non-rename
#                  change, so a plain one-path-per-line list is still valid
#                  input.
#   EVIDENCE       file, the PR body concatenated with the content of any
#                  docs/hardware-validation/*.md the PR adds or edits
#   LABELS         file, one PR label per line (may be empty)
#   PR_TITLE       the PR title, as a single argument
#
# Report is written to stdout as GitHub-flavoured Markdown, suitable for
# $GITHUB_STEP_SUMMARY.

set -euo pipefail

GATE_DOC="docs/update-survivability-gate.md"

# --- Protected paths ---------------------------------------------------------
# Bash glob patterns, matched against each changed path with [[ == ]].
#
# The explicitly-named entries are the current update path. The trailing
# broad patterns exist because the named list is a hand-maintained inventory
# of danger and a NEW file is invisible to it until someone remembers to add
# it -- exactly the class of omission this gate is about. The broad patterns
# cover the naming conventions the update path actually uses, so a future
# src/network/FirmwareSignature.cpp is caught on the day it is written.
PROTECTED_PATTERNS=(
  # -- named: image install and validation --
  "src/network/FirmwareFlasher.*"
  "src/network/OtaUpdater.*"
  "src/network/OtaBootSwitch.*"
  "src/network/FirmwareBoardTag.*"
  "src/network/FirmwareImageIdentity.h"
  "src/network/FirmwareUpdatePolicy.h"
  # -- named: rollback / recovery --
  "src/OtaRollback*"
  # -- named: the two user-facing install activities --
  "src/activities/settings/SdFirmwareUpdateActivity.*"
  "src/activities/settings/OtaUpdateActivity.*"
  # -- named: flash geometry and release machinery --
  "partitions.csv"
  ".github/workflows/release*.yml"
  # -- broad: catch newly added files following the same conventions --
  "src/network/Firmware*"
  "src/network/Ota*"
  "src/Ota*"
  "src/activities/settings/Ota*"
  "src/activities/settings/SdFirmware*"
  "scripts/*sign*"
  "scripts/*hash*"
  "scripts/*checksum*"
  "scripts/*digest*"
)

# --- Documented carve-outs ---------------------------------------------------
# Checked BEFORE the protected list, and deliberately kept tiny. A gate that
# hard-blocks work it has nothing to say about is a gate people learn to route
# around, and a routed-around gate protects nothing -- so a genuine false
# positive gets an explicit, reviewable exclusion here rather than a habit of
# merging past a red check.
#
# release-fonts.yml publishes SD-card font packs. It matches
# `.github/workflows/release*.yml` by name only; it touches no firmware image,
# no partition and no boot slot, so no amount of two-slot install testing
# could say anything about a change to it. Any NEW exclusion must clear the
# same bar: the file cannot influence what image is built, validated, written
# to a slot, or selected as bootable.
EXCLUDED_PATTERNS=(
  ".github/workflows/release-fonts.yml"
)

# --- Required matrix rows ----------------------------------------------------
# Case IDs from docs/update-survivability-gate.md. Every one must appear in the
# evidence with a verdict. Keep in sync with that document's table.
REQUIRED_CASES=(
  "US-0"   # recovery precondition proven before flashing
  "US-1"   # A @ app0 -> install B -> app1
  "US-2"   # B @ app1 -> install C -> app0
  "US-3"   # C @ app0 -> install B -> app1
  "US-4"   # self-reinstall (or N/A where unsupported)
  "US-5"   # wrong-chip image rejected, neither slot erased
  "US-6"   # wrong-board image rejected, never selected bootable
  "US-7"   # corrupted / truncated image rejected before erase
  "US-8"   # HTTP failure BEFORE download, fallback intact
  "US-9"   # HTTP failure DURING download, fallback intact
  "US-10"  # power-loss simulation at documented safe stages
  "US-11"  # after every failure, the previous slot still boots
)

die_undetermined() {
  echo "update-survivability-gate.sh: cannot determine verdict -- failing closed: $1" >&2
  exit 2
}

# --- Inputs ------------------------------------------------------------------
if [ "$#" -ne 4 ]; then
  die_undetermined "expected 4 arguments (CHANGED_FILES EVIDENCE LABELS PR_TITLE), got $#"
fi

CHANGED_FILES_PATH="$1"
EVIDENCE_PATH="$2"
LABELS_PATH="$3"
PR_TITLE="$4"

[ -r "$CHANGED_FILES_PATH" ] || die_undetermined "changed-file list '$CHANGED_FILES_PATH' is not readable"
[ -r "$EVIDENCE_PATH" ] || die_undetermined "evidence file '$EVIDENCE_PATH' is not readable"
[ -r "$LABELS_PATH" ] || die_undetermined "labels file '$LABELS_PATH' is not readable"

CHANGED_FILES="$(cat "$CHANGED_FILES_PATH")"
# A pull request with an empty changed-file list is not a pass, it is a failed
# enumeration -- the API call returned nothing, was truncated, or paginated
# past its cap. Refusing to judge it is the whole point of exit 2.
if [ -z "${CHANGED_FILES//[[:space:]]/}" ]; then
  die_undetermined "changed-file list is empty (a PR always changes at least one file; treat as a failed enumeration, not a clean diff)"
fi

EVIDENCE="$(cat "$EVIDENCE_PATH")"
LABELS="$(cat "$LABELS_PATH")"

# --- 1. Which protected paths does this PR touch? ----------------------------
# One path in isolation: protected when it matches PROTECTED_PATTERNS and is
# not carved out by EXCLUDED_PATTERNS. Exclusions are evaluated per PATH, not
# per record, so a rename FROM a carved-out path INTO a protected one is still
# judged on the protected side.
is_protected_path() {
  local candidate="$1" pattern
  [ -z "$candidate" ] && return 1
  for pattern in "${EXCLUDED_PATTERNS[@]}"; do
    # shellcheck disable=SC2053  # deliberate glob match, not a string compare
    if [[ "$candidate" == $pattern ]]; then
      return 1
    fi
  done
  for pattern in "${PROTECTED_PATTERNS[@]}"; do
    # shellcheck disable=SC2053  # deliberate glob match, not a string compare
    if [[ "$candidate" == $pattern ]]; then
      return 0
    fi
  done
  return 1
}

# Both sides of a rename are classified, and a hit on EITHER side blocks:
#
#   protected -> unprotected  a rename AWAY from the update path. The moved
#                             content is still the updater, and the
#                             destination matches nothing, so destination-only
#                             matching sees an ordinary new file. This is the
#                             reported bypass.
#   unprotected -> protected  a rename INTO the update path. Whatever content
#                             lands at src/network/OtaUpdater.cpp IS the OTA
#                             updater from that commit on, whatever it was
#                             called yesterday -- functionally a rewrite of a
#                             protected file, and it must block for exactly
#                             the reason a modification does.
#
# TOUCHED holds display entries, so a rename names BOTH paths and says which
# direction tripped the gate; an author looking at a diff that no longer
# contains the protected path needs to be told where it went.
TOUCHED=""
RENAME_HIT=false
while IFS=$'\t' read -r new_path old_path; do
  old_path="${old_path:-}"
  [ -z "$new_path" ] && [ -z "$old_path" ] && continue

  new_protected=false
  old_protected=false
  if is_protected_path "$new_path"; then new_protected=true; fi
  if is_protected_path "$old_path"; then old_protected=true; fi
  if [ "$new_protected" = false ] && [ "$old_protected" = false ]; then
    continue
  fi

  if [ -z "$old_path" ]; then
    TOUCHED="${TOUCHED}${new_path}"$'\n'
    continue
  fi

  RENAME_HIT=true
  if [ "$new_protected" = true ] && [ "$old_protected" = true ]; then
    TOUCHED="${TOUCHED}${old_path} -> ${new_path}  (RENAMED, both paths protected)"$'\n'
  elif [ "$old_protected" = true ]; then
    TOUCHED="${TOUCHED}${old_path} -> ${new_path}  (RENAMED AWAY from a protected path: matched on the SOURCE path -- the destination matches no pattern)"$'\n'
  else
    TOUCHED="${TOUCHED}${old_path} -> ${new_path}  (RENAMED INTO a protected path: matched on the DESTINATION path)"$'\n'
  fi
done <<<"$CHANGED_FILES"
TOUCHED="$(printf '%s' "$TOUCHED" | sed '/^$/d' | awk '!seen[$0]++')"

# --- 2. Does the PR present itself as UI work? -------------------------------
# Informational only. Used to escalate the failure message for the scope-creep
# shape that caused the incident (a UI RC that quietly moved the install path);
# it never changes the verdict.
ui_presentation() {
  local title_lc
  title_lc="$(printf '%s' "$PR_TITLE" | tr '[:upper:]' '[:lower:]')"
  case "$title_lc" in
    *"(ui)"*|*"(ux)"*|*"(touch)"*|*touch*|*ui/*) echo "title"; return 0 ;;
  esac

  local label label_lc
  while IFS= read -r label; do
    [ -z "$label" ] && continue
    label_lc="$(printf '%s' "$label" | tr '[:upper:]' '[:lower:]')"
    case "$label_lc" in
      ui|ux|touch|frontend|"ui/ux"|ui:*|"ui/"*|hardware-test) echo "label"; return 0 ;;
    esac
  done <<<"$LABELS"

  # Path heuristic: at least half the diff is UI surface. Counted per RECORD,
  # so a rename is one changed file rather than two; either side counting as
  # UI surface makes the record UI.
  local total=0 ui=0 new_path old_path
  while IFS=$'\t' read -r new_path old_path; do
    old_path="${old_path:-}"
    [ -z "$new_path" ] && [ -z "$old_path" ] && continue
    total=$((total + 1))
    case "$new_path" in
      src/activities/*|lib/UITheme/*|lib/GfxRenderer/*|lib/I18n/*|lib/EpdFont/*)
        ui=$((ui + 1))
        continue
        ;;
    esac
    case "$old_path" in
      src/activities/*|lib/UITheme/*|lib/GfxRenderer/*|lib/I18n/*|lib/EpdFont/*) ui=$((ui + 1)) ;;
    esac
  done <<<"$CHANGED_FILES"
  if [ "$total" -gt 0 ] && [ $((ui * 2)) -ge "$total" ]; then
    echo "diff-shape"
    return 0
  fi

  echo ""
}
UI_SIGNAL="$(ui_presentation)"

# --- 3. Report evaluation ----------------------------------------------------
# Only reached when a protected path is touched. Every REQUIRED_CASES id must
# appear in the evidence on a line carrying a verdict; PASS and N/A are
# accepted, FAIL and BLOCKED are not, and a missing id is not.
MISSING_CASES=""
FAILED_CASES=""
evaluate_matrix() {
  local id line verdict
  for id in "${REQUIRED_CASES[@]}"; do
    # Anchor on the id followed by a non-alphanumeric, so US-1 never matches
    # US-10 or US-11.
    line="$(grep -E "(^|[^A-Za-z0-9-])${id}([^0-9]|$)" <<<"$EVIDENCE" | head -n 1 || true)"
    if [ -z "$line" ]; then
      MISSING_CASES="${MISSING_CASES}${id}"$'\n'
      continue
    fi
    verdict="$(grep -oiE '\b(PASS|FAIL|BLOCKED|N/A)\b' <<<"$line" | tail -n 1 | tr '[:lower:]' '[:upper:]' || true)"
    case "$verdict" in
      PASS|"N/A") : ;;
      FAIL|BLOCKED) FAILED_CASES="${FAILED_CASES}${id}: ${verdict}"$'\n' ;;
      *) MISSING_CASES="${MISSING_CASES}${id} (no PASS / FAIL / N/A verdict on its row)"$'\n' ;;
    esac
  done
}

MISSING_FIELDS=""
evaluate_fields() {
  # Test device: must be named, and must not be blank.
  if ! grep -qiE '^[[:space:]>|*-]*test[ -]device[[:space:]]*[:|][[:space:]]*[^[:space:]|]' <<<"$EVIDENCE"; then
    MISSING_FIELDS="${MISSING_FIELDS}Test device: <designated unit, never the only working device>"$'\n'
  fi
  # Build SHAs: the line must exist AND carry at least one 7+ hex commit id, so
  # a placeholder like "Build SHAs: TBD" does not satisfy it.
  if ! grep -iE '^[[:space:]>|*-]*build[ -]sha' <<<"$EVIDENCE" | grep -qE '\b[0-9a-fA-F]{7,40}\b'; then
    MISSING_FIELDS="${MISSING_FIELDS}Build SHAs: <A> <B> <C> (7-40 hex each)"$'\n'
  fi
  # The matrix block itself must be labelled, so the ids below are demonstrably
  # a transcribed result table and not incidental prose.
  if ! grep -qiE 'survivability[ -]matrix' <<<"$EVIDENCE"; then
    MISSING_FIELDS="${MISSING_FIELDS}Survivability matrix: <the US-0..US-11 result rows>"$'\n'
  fi
}

# --- 4. Verdict --------------------------------------------------------------
if [ -z "$TOUCHED" ]; then
  {
    echo "## Update Survivability Gate"
    echo
    echo ":white_check_mark: **Not applicable.** This PR touches no firmware update path, boot slot, partition or release file."
    echo
    echo "Gate definition: \`$GATE_DOC\`"
  }
  exit 0
fi

evaluate_fields
evaluate_matrix

if [ -z "$MISSING_FIELDS" ] && [ -z "$MISSING_CASES" ] && [ -z "$FAILED_CASES" ]; then
  {
    echo "## Update Survivability Gate"
    echo
    echo ":white_check_mark: **Passed.** This PR touches the firmware update path and carries a complete hardware validation report."
    echo
    echo "### Protected files touched"
    echo '```'
    echo "$TOUCHED"
    echo '```'
    echo
    echo "All ${#REQUIRED_CASES[@]} rows of the survivability matrix are recorded with a PASS or N/A verdict. The reviewer still owns judging whether the recorded slots, image SHA-256 values and boot results are real -- this job checks that they were WRITTEN DOWN, not that they are true."
    echo
    echo "Gate definition: \`$GATE_DOC\`"
  }
  exit 0
fi

{
  echo "## Update Survivability Gate"
  echo
  echo ":x: **Blocked.** This PR changes the firmware update path and has no complete hardware validation report."
  echo
  echo "### Protected files touched"
  echo '```'
  echo "$TOUCHED"
  echo '```'
  echo

  if [ "$RENAME_HIT" = true ]; then
    echo "### A rename is what put a protected path in this list"
    echo
    echo "One of the entries above is a **rename**, shown as \`source -> destination\`. Both paths of every rename are checked, which is why this gate can block on a path your diff no longer contains:"
    echo
    echo "- **Renamed away from a protected path** -- the file that was the updater still is the updater; it is just called something else now. Its destination matches no pattern, so matching the destination alone would have reported this PR as \"Not applicable\"."
    echo "- **Renamed into a protected path** -- whatever content now lives at that pathname is the update path from this commit on, regardless of what the file was called before."
    echo
    echo "Renaming, splitting or moving these files does not make the change safer to ship; if anything it makes the two-slot matrix more important, because the install path the device runs after this PR is code that has never completed a full install cycle under its new name."
    echo
  fi

  if [ -n "$UI_SIGNAL" ]; then
    echo "### :rotating_light: This PR presents as UI work (detected via: $UI_SIGNAL)"
    echo
    echo "That is the exact shape of the incident this gate exists for: a UI/touch RC that also moved the firmware installation path, shipped after a single successful install, and left the only X4 Pro update-locked. A UI PR that reaches into these files is not a UI PR."
    echo
    echo "If the update-path edits are not essential to the UI change, **split them out** -- that is the cheapest way through this gate."
    echo
  fi

  if [ -n "$MISSING_FIELDS" ]; then
    echo "### Missing report fields"
    echo
    echo "Add these to the PR description (or to a \`docs/hardware-validation/*.md\` added by this PR):"
    echo '```'
    printf '%s' "$MISSING_FIELDS"
    echo '```'
    echo
  fi

  if [ -n "$MISSING_CASES" ]; then
    echo "### Missing matrix rows"
    echo
    echo "Each row needs a \`PASS\`, \`FAIL\` or \`N/A\` verdict:"
    echo '```'
    printf '%s' "$MISSING_CASES"
    echo '```'
    echo
  fi

  if [ -n "$FAILED_CASES" ]; then
    echo "### Rows that did not pass"
    echo '```'
    printf '%s' "$FAILED_CASES"
    echo '```'
    echo
    echo "A recorded FAIL or BLOCKED row is a correct, useful result -- it just is not shippable. Fix the firmware, re-run the matrix, update the report."
    echo
  fi

  echo "### What this gate will NOT accept as a substitute"
  echo
  echo "- A green build on all four board environments"
  echo "- A passing host test suite, however large"
  echo "- \`cppcheck\`, CodeQL, or a clean CodeRabbit review"
  echo "- A single successful installation on real hardware"
  echo
  echo "All of those passed on the change that bricked the device. None of them runs on hardware AND exercises an install *cycle*. Only a designated test unit completing the two-slot matrix does."
  echo
  echo "### How to unblock"
  echo
  echo "1. Read **\`$GATE_DOC\`** and run the matrix on a designated test device -- never the only working unit."
  echo "2. Record running slot, target slot, image SHA-256 and boot result for every transition."
  echo "3. Paste the filled-in results block from that document into this PR's description, or add the report under \`docs/hardware-validation/\`."
  echo
  echo "Gate definition: \`$GATE_DOC\`"
}
exit 1
