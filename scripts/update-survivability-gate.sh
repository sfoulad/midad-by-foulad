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
# Two independent classifications
# -------------------------------
# 1. FIRMWARE UPDATE PATH (PROTECTED_PATTERNS below). Cleared by a hardware
#    validation report: the US-0..US-11 matrix, run on a designated device.
# 2. GATE SELF-MODIFICATION (GATE_SELF_PATHS below) -- a separate, fail-closed
#    security-boundary classification for a PR that changes the gate's own
#    workflow, evaluator, self-test suite or specification. It is NOT cleared
#    by a hardware matrix (running US-0..US-11 on a device says nothing about
#    a change to a CI script) and it is NOT cleared by anything the PR author
#    writes: a body line, a title convention or a label is text the author
#    controls, so it authorizes nothing. It is cleared ONLY by an APPROVED
#    GitHub review from OWNER / MEMBER / COLLABORATOR whose commit_id is the
#    PR's CURRENT head SHA -- authorization that comes from GitHub's own
#    permission model, and that a later push invalidates.
#
# Both classifications can apply to one PR; both must then be satisfied.
#
# Fail-closed contract
# --------------------
# Exit 0: neither classification applies, OR every classification that applies
#         is satisfied.
# Exit 1: a protected path was touched and the report is missing, incomplete,
#         or records a FAIL/BLOCKED row; and/or a gate file was touched with
#         no qualifying maintainer approval on the current head SHA.
# Exit 2: the gate could not DETERMINE the answer -- an unreadable or empty
#         changed-file list, an unreadable evidence/labels/reviews file, a
#         missing head SHA. The caller MUST treat exit 2 as a failure, never
#         as a pass. Reading a missing input as "nothing to check" is the
#         identical fail-open mistake that produced the incident this gate
#         exists to prevent.
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
#   update-survivability-gate.sh CHANGED_FILES EVIDENCE LABELS PR_TITLE \
#                                REVIEWS HEAD_SHA
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
#   REVIEWS        file, one PR REVIEW per line as
#                  `state<TAB>author_association<TAB>commit_id` -- the fields
#                  of the GitHub pull-reviews API. May be empty (no reviews);
#                  an empty file is a determinate "nobody has approved yet",
#                  not an undetermined input.
#   HEAD_SHA       the PR's current head SHA, as a single argument. An
#                  approval is only honoured when it was given against THIS
#                  commit, so a push after approval re-blocks the gate.
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

# --- The gate's own files (second, independent classification) ---------------
# A PR that changes the gate's workflow, its evaluator, its self-test suite or
# its specification matches NONE of the protected firmware patterns above. With
# only one classification the gate would report "Not applicable" on such a PR
# and a weakening of the evaluator would merge behind a green check -- the gate
# would not protect itself.
#
# These are deliberately NOT added to PROTECTED_PATTERNS. Demanding a
# US-0..US-11 hardware matrix for a change to a CI script is both meaningless
# (no device transition can say anything about a shell script) and
# disproportionate enough to make the gate something people route around --
# which this file's own carve-out comment warns against. They get their own
# clearing mechanism instead; see maintainer_approval_state().
#
# The self-test suite is included for the same reason scripts/thin-fork-guard.sh
# treats scripts/test-thin-fork-guard.sh as a security-boundary file: the
# workflow EXECUTES the self-test from the base checkout before it trusts the
# evaluator's verdict, so it is trusted executable code, not documentation. The
# specification is included because it is the contract the parser is held to --
# §5's results template and this script's row parser are locked together by a
# test, and a silent edit to one side of that lock is exactly the invisible
# change this classification exists to prevent.
GATE_SELF_PATHS=(
  ".github/workflows/update-survivability-gate.yml"
  "scripts/update-survivability-gate.sh"
  "scripts/test-update-survivability-gate.sh"
  "docs/update-survivability-gate.md"
)

# Author associations that count as maintainer authority on this repository.
# GitHub computes author_association itself from the actor's permission on the
# repo; it is not settable by the PR author.
MAINTAINER_ASSOCIATIONS=(OWNER MEMBER COLLABORATOR)

# --- Required matrix rows ----------------------------------------------------
# Case IDs from docs/update-survivability-gate.md. Every one must appear in the
# evidence with a verdict. Keep in sync with that document's table.
# Erase expectations are CHANNEL-SPECIFIC; see the matrix in that document for
# which half of each case applies to SD and which to streaming OTA.
REQUIRED_CASES=(
  "US-0"   # recovery precondition proven before flashing
  "US-1"   # A @ app0 -> install B -> app1
  "US-2"   # B @ app1 -> install C -> app0
  "US-3"   # C @ app0 -> install B -> app1
  "US-4"   # self-reinstall (or N/A where unsupported) -- the ONLY N/A case
  "US-5"   # wrong-chip image rejected: SD before erase; OTA never made bootable
  "US-6"   # wrong-board image rejected, never selected bootable
  "US-7"   # corrupted / truncated image rejected: SD before erase
  "US-8"   # HTTP failure BEFORE download; running slot still boots
  "US-9"   # HTTP failure DURING download; running slot still boots
  "US-10"  # power-loss simulation at documented safe stages
  "US-11"  # after every failure, the previous slot still boots
)

# The one case where `N/A` is a legitimate verdict, per §5 of the gate
# document. Every other row must record a real result.
NA_PERMITTED_CASES=(
  "US-4"
)

# Matrix rows are markdown table rows with these cells, in this order. The
# parser reads them BY POSITION and §5 of the gate document publishes exactly
# this column order; scripts/test-update-survivability-gate.sh fills in the
# document's own template and asserts this parser accepts it, so the two can
# never drift apart silently.
#
#   | ID | Running slot | Target slot | Image SHA-256 | Boot result | Verdict | Notes |
#      2        3              4              5             6           7        8
# (cell 2, the ID, is matched textually when the row is located.)
ROW_CELL_RUNNING=3
ROW_CELL_TARGET=4
ROW_CELL_SHA=5
ROW_CELL_BOOT=6
ROW_CELL_VERDICT=7

die_undetermined() {
  echo "update-survivability-gate.sh: cannot determine verdict -- failing closed: $1" >&2
  exit 2
}

# --- Inputs ------------------------------------------------------------------
if [ "$#" -ne 6 ]; then
  die_undetermined "expected 6 arguments (CHANGED_FILES EVIDENCE LABELS PR_TITLE REVIEWS HEAD_SHA), got $#"
fi

CHANGED_FILES_PATH="$1"
EVIDENCE_PATH="$2"
LABELS_PATH="$3"
PR_TITLE="$4"
REVIEWS_PATH="$5"
HEAD_SHA="$6"

[ -r "$CHANGED_FILES_PATH" ] || die_undetermined "changed-file list '$CHANGED_FILES_PATH' is not readable"
[ -r "$EVIDENCE_PATH" ] || die_undetermined "evidence file '$EVIDENCE_PATH' is not readable"
[ -r "$LABELS_PATH" ] || die_undetermined "labels file '$LABELS_PATH' is not readable"
# An unreadable reviews file is an undetermined input, not "nobody approved":
# the difference decides whether a gate-self-modification PR is judged at all.
[ -r "$REVIEWS_PATH" ] || die_undetermined "reviews file '$REVIEWS_PATH' is not readable"
# Without the head SHA an approval cannot be bound to a commit, so an approval
# from before the last push would be indistinguishable from a current one.
if [ -z "${HEAD_SHA//[[:space:]]/}" ]; then
  die_undetermined "head SHA is empty (an approval must be bound to the commit it was given against)"
fi

CHANGED_FILES="$(cat "$CHANGED_FILES_PATH")"
# A pull request with an empty changed-file list is not a pass, it is a failed
# enumeration -- the API call returned nothing, was truncated, or paginated
# past its cap. Refusing to judge it is the whole point of exit 2.
if [ -z "${CHANGED_FILES//[[:space:]]/}" ]; then
  die_undetermined "changed-file list is empty (a PR always changes at least one file; treat as a failed enumeration, not a clean diff)"
fi

EVIDENCE="$(cat "$EVIDENCE_PATH")"
LABELS="$(cat "$LABELS_PATH")"
REVIEWS="$(cat "$REVIEWS_PATH")"

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

# --- 1b. Does this PR modify the gate itself? --------------------------------
# Exact-path membership (not a glob) on BOTH sides of every rename, for the
# same reason the protected list checks both: renaming the evaluator out of the
# way is functionally a rewrite of it, and renaming something INTO the
# evaluator's pathname is what the workflow will execute from the next merge
# onward.
is_gate_self_path() {
  local candidate="$1" gate_path
  [ -z "$candidate" ] && return 1
  for gate_path in "${GATE_SELF_PATHS[@]}"; do
    [ "$candidate" = "$gate_path" ] && return 0
  done
  return 1
}

GATE_SELF_TOUCHED=""
while IFS=$'\t' read -r new_path old_path; do
  old_path="${old_path:-}"
  [ -z "$new_path" ] && [ -z "$old_path" ] && continue

  if is_gate_self_path "$new_path"; then
    if [ -n "$old_path" ] && [ "$old_path" != "$new_path" ]; then
      GATE_SELF_TOUCHED="${GATE_SELF_TOUCHED}${old_path} -> ${new_path}  (RENAMED INTO the gate's own files)"$'\n'
    else
      GATE_SELF_TOUCHED="${GATE_SELF_TOUCHED}${new_path}"$'\n'
    fi
    continue
  fi
  if is_gate_self_path "$old_path"; then
    GATE_SELF_TOUCHED="${GATE_SELF_TOUCHED}${old_path} -> ${new_path}  (RENAMED AWAY from the gate's own files)"$'\n'
  fi
done <<<"$CHANGED_FILES"
GATE_SELF_TOUCHED="$(printf '%s' "$GATE_SELF_TOUCHED" | sed '/^$/d' | awk '!seen[$0]++')"

# Clearing a gate self-modification requires authorization GitHub itself
# computes, never text the PR author supplies. An APPROVED review whose
# author_association is OWNER / MEMBER / COLLABORATOR is a permission fact; a
# body line, a title convention or a label is not, because the same person who
# wrote the change writes all three. Binding the approval to the current head
# SHA means a push after approval re-blocks the gate rather than inheriting it.
#
# Echoes one of: approved / stale / unauthorized / none.
maintainer_approval_state() {
  local state assoc commit assoc_uc state_uc allowed a best="none"
  while IFS=$'\t' read -r state assoc commit; do
    [ -z "$state" ] && continue
    state_uc="$(printf '%s' "$state" | tr '[:lower:]' '[:upper:]')"
    [ "$state_uc" = "APPROVED" ] || continue

    assoc_uc="$(printf '%s' "$assoc" | tr '[:lower:]' '[:upper:]')"
    allowed=false
    for a in "${MAINTAINER_ASSOCIATIONS[@]}"; do
      [ "$assoc_uc" = "$a" ] && allowed=true && break
    done
    if [ "$allowed" = false ]; then
      [ "$best" = "none" ] && best="unauthorized"
      continue
    fi

    if [ "$commit" != "$HEAD_SHA" ]; then
      [ "$best" != "approved" ] && best="stale"
      continue
    fi
    echo "approved"
    return 0
  done <<<"$REVIEWS"
  echo "$best"
}

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
# Only reached when a protected path is touched.
#
# A result is a STRUCTURED ROW, not a mention. The earlier "any line containing
# the id and any verdict word" rule accepted a sentence in the PR body -- and
# accepted `N/A` for every case, although §5 permits it for US-4 alone. A row
# must be a markdown table row whose first cell is exactly the case id and
# which records all four documented transition fields (running slot, target
# slot, image SHA-256, boot result) plus a verdict. Write `-` where a field
# genuinely does not apply; a blank or `TBD` cell is an unfilled template.

# Trimmed cell N of a markdown table row. Cell 1 is whatever precedes the
# leading pipe (empty, or a blockquote marker), so the ID is cell 2.
row_cell() {
  awk -F'|' -v n="$2" '{
    v = (n <= NF) ? $n : ""
    gsub(/^[[:space:]`*]+|[[:space:]`*]+$/, "", v)
    print v
  }' <<<"$1"
}

# A cell is filled when it carries something other than whitespace and is not a
# leftover template placeholder.
cell_is_filled() {
  local v_uc
  [ -z "$1" ] && return 1
  v_uc="$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')"
  case "$v_uc" in
    TBD|TODO|"?"|"<>") return 1 ;;
  esac
  case "$1" in
    "<"*">") return 1 ;;
  esac
  return 0
}

na_permitted_for() {
  local id
  for id in "${NA_PERMITTED_CASES[@]}"; do
    [ "$1" = "$id" ] && return 0
  done
  return 1
}

MISSING_CASES=""
FAILED_CASES=""
evaluate_matrix() {
  local id row verdict running target sha boot missing_fields
  for id in "${REQUIRED_CASES[@]}"; do
    # The first cell must be EXACTLY the id, so US-1 can never be satisfied by
    # US-10 or US-11 and prose mentioning an id is never mistaken for a result.
    row="$(grep -E "^[[:space:]>]*\|[[:space:]]*${id}[[:space:]]*\|" <<<"$EVIDENCE" | head -n 1 || true)"
    if [ -z "$row" ]; then
      MISSING_CASES="${MISSING_CASES}${id}"$'\n'
      continue
    fi

    verdict="$(row_cell "$row" "$ROW_CELL_VERDICT" | tr '[:lower:]' '[:upper:]')"
    case "$verdict" in
      PASS|FAIL|BLOCKED|"N/A") : ;;
      *)
        MISSING_CASES="${MISSING_CASES}${id} (no PASS / FAIL / N/A verdict in the Verdict cell -- the row must be a full matrix row, not a mention)"$'\n'
        continue
        ;;
    esac

    if [ "$verdict" = "N/A" ] && ! na_permitted_for "$id"; then
      FAILED_CASES="${FAILED_CASES}${id}: N/A is not permitted for this case (only ${NA_PERMITTED_CASES[*]} may be N/A)"$'\n'
      continue
    fi

    if [ "$verdict" = "FAIL" ] || [ "$verdict" = "BLOCKED" ]; then
      FAILED_CASES="${FAILED_CASES}${id}: ${verdict}"$'\n'
      continue
    fi

    running="$(row_cell "$row" "$ROW_CELL_RUNNING")"
    target="$(row_cell "$row" "$ROW_CELL_TARGET")"
    sha="$(row_cell "$row" "$ROW_CELL_SHA")"
    boot="$(row_cell "$row" "$ROW_CELL_BOOT")"
    missing_fields=""
    cell_is_filled "$running" || missing_fields="${missing_fields}running slot, "
    cell_is_filled "$target" || missing_fields="${missing_fields}target slot, "
    cell_is_filled "$sha" || missing_fields="${missing_fields}image SHA-256, "
    cell_is_filled "$boot" || missing_fields="${missing_fields}boot result, "
    if [ -n "$missing_fields" ]; then
      MISSING_CASES="${MISSING_CASES}${id} (row does not record: ${missing_fields%, } -- write \`-\` where a field genuinely does not apply)"$'\n'
    fi
  done
}

# The 7-40 hex value labelled with a given letter on the Build SHAs line.
# Echoes nothing when that label carries no real hash.
labelled_build_sha() {
  # `|| true`: no match is an ordinary answer here, and under `set -o pipefail`
  # an unmatched grep would otherwise abort the whole script.
  local found
  found="$(printf '%s' "$1" \
    | grep -oiE "(^|[^0-9A-Za-z])$2[[:space:]]*=[[:space:]]*[\`'\"*]*[0-9a-fA-F]{7,40}([^0-9A-Za-z]|$)" \
    | head -n 1 \
    | grep -oE '[0-9a-fA-F]{7,40}' \
    | tail -n 1 \
    | tr '[:upper:]' '[:lower:]' || true)"
  printf '%s' "$found"
}

MISSING_FIELDS=""
evaluate_fields() {
  local sha_line sha_a sha_b sha_c
  # Test device: must be named, and must not be blank.
  # The value must be a real name: `<model, serial/MAC, ...>` straight out of
  # the template is an unfilled placeholder, not a designated unit.
  if ! grep -qiE '^[[:space:]>|*-]*test[ -]device[[:space:]]*[:|][[:space:]]*[^[:space:]|<]' <<<"$EVIDENCE"; then
    MISSING_FIELDS="${MISSING_FIELDS}Test device: <designated unit, never the only working device>"$'\n'
  fi

  # Build SHAs: §4 requires THREE builds -- A (known-good baseline), B (the RC)
  # and C (a third build distinguishable from B, so the app1 -> app0 hop is
  # tested with an image that is not already resident). All three must be real
  # hashes and all three must differ; "Build SHAs: TBD", a single hash, or
  # A=B=C are each a matrix that cannot have been run as documented.
  sha_line="$(grep -iE '^[[:space:]>|*-]*build[ -]sha' <<<"$EVIDENCE" | head -n 1 || true)"
  sha_a="$(labelled_build_sha "$sha_line" A)"
  sha_b="$(labelled_build_sha "$sha_line" B)"
  sha_c="$(labelled_build_sha "$sha_line" C)"
  if [ -z "$sha_a" ] || [ -z "$sha_b" ] || [ -z "$sha_c" ]; then
    MISSING_FIELDS="${MISSING_FIELDS}Build SHAs: A=<commit> B=<commit> C=<commit> (7-40 hex each; all three are required)"$'\n'
  elif [ "$sha_a" = "$sha_b" ] || [ "$sha_a" = "$sha_c" ] || [ "$sha_b" = "$sha_c" ]; then
    MISSING_FIELDS="${MISSING_FIELDS}Build SHAs: A, B and C must be THREE DISTINCT builds (C exists so the app1 -> app0 hop uses an image that is not already resident)"$'\n'
  fi

  # The matrix block itself must be labelled, so the ids below are demonstrably
  # a transcribed result table and not incidental prose.
  if ! grep -qiE 'survivability[ -]matrix' <<<"$EVIDENCE"; then
    MISSING_FIELDS="${MISSING_FIELDS}Survivability matrix: <the US-0..US-11 result rows>"$'\n'
  fi
}

# --- 4. Verdict --------------------------------------------------------------
GATE_SELF_APPROVAL="none"
if [ -n "$GATE_SELF_TOUCHED" ]; then
  GATE_SELF_APPROVAL="$(maintainer_approval_state)"
fi

if [ -z "$TOUCHED" ] && [ -z "$GATE_SELF_TOUCHED" ]; then
  {
    echo "## Update Survivability Gate"
    echo
    echo ":white_check_mark: **Not applicable.** This PR touches no firmware update path, boot slot, partition or release file, and does not modify the gate itself."
    echo
    echo "Gate definition: \`$GATE_DOC\`"
  }
  exit 0
fi

if [ -n "$TOUCHED" ]; then
  evaluate_fields
  evaluate_matrix
fi

MATRIX_OK=false
if [ -z "$MISSING_FIELDS" ] && [ -z "$MISSING_CASES" ] && [ -z "$FAILED_CASES" ]; then
  MATRIX_OK=true
fi
SELF_MOD_OK=false
if [ -z "$GATE_SELF_TOUCHED" ] || [ "$GATE_SELF_APPROVAL" = "approved" ]; then
  SELF_MOD_OK=true
fi

if [ "$MATRIX_OK" = true ] && [ "$SELF_MOD_OK" = true ]; then
  {
    echo "## Update Survivability Gate"
    echo
    echo ":white_check_mark: **Passed.**"
    echo
    if [ -n "$TOUCHED" ]; then
      echo "### Protected files touched"
      echo '```'
      echo "$TOUCHED"
      echo '```'
      echo
      echo "All ${#REQUIRED_CASES[@]} rows of the survivability matrix are recorded with a permitted verdict and all four transition fields. The reviewer still owns judging whether the recorded slots, image SHA-256 values and boot results are real -- this job checks that they were WRITTEN DOWN, not that they are true."
      echo
    fi
    if [ -n "$GATE_SELF_TOUCHED" ]; then
      echo "### Gate self-modification, approved"
      echo '```'
      echo "$GATE_SELF_TOUCHED"
      echo '```'
      echo
      echo "Cleared by an APPROVED review from a maintainer (OWNER / MEMBER / COLLABORATOR) recorded against the current head SHA \`$HEAD_SHA\`. Pushing another commit invalidates that approval and re-blocks this gate."
      echo
    fi
    echo "Gate definition: \`$GATE_DOC\`"
  }
  exit 0
fi

{
  echo "## Update Survivability Gate"
  echo
  echo ":x: **Blocked.**"
  echo

  if [ "$SELF_MOD_OK" = false ]; then
    echo "### :lock: This PR modifies the update survivability gate itself"
    echo
    echo "Gate files changed by this PR:"
    echo '```'
    echo "$GATE_SELF_TOUCHED"
    echo '```'
    echo
    echo "This is a **separate classification** from the hardware matrix, and a hardware matrix does **not** clear it: running US-0..US-11 on a device says nothing about a change to a CI workflow or a shell script."
    echo
    echo "**It is also not cleared by anything written in this PR.** A line in the description, a title convention and a label are all text the PR author controls, so none of them is authorization. The only thing that clears it is an **APPROVED GitHub review from a maintainer** -- \`author_association\` of ${MAINTAINER_ASSOCIATIONS[*]} -- recorded against **this PR's current head SHA** (\`$HEAD_SHA\`). Pushing a new commit after approval invalidates it, so an approval can never be carried across a change."
    echo
    case "$GATE_SELF_APPROVAL" in
      stale)
        echo "Current state: an approving maintainer review exists, but it was given against an **earlier commit**. Re-approve the current head."
        ;;
      unauthorized)
        echo "Current state: an approving review exists, but its \`author_association\` is not one of ${MAINTAINER_ASSOCIATIONS[*]}. GitHub computes that value from the reviewer's permission on this repository; it cannot be set by the PR."
        ;;
      *)
        echo "Current state: no approving maintainer review on the current head SHA."
        ;;
    esac
    echo
    echo "**This is a visibility control, not a prevention control.** \`pull_request_target\` already guarantees that the BASE branch's copy of the workflow and evaluator is what judges this PR, so nothing changed here can weaken the verdict on this PR. What that property does *not* do is make such a change noticeable: a PR touching only these files matches no protected firmware path, so without this classification the gate would report \"Not applicable\" and an edit that weakens the evaluator for every FUTURE PR would merge behind a green check. The point is that it can never be invisible."
    echo
  fi

  if [ -n "$TOUCHED" ] && [ "$MATRIX_OK" = false ]; then
    echo "### This PR changes the firmware update path and has no complete hardware validation report"
    echo
    echo "#### Protected files touched"
    echo '```'
    echo "$TOUCHED"
    echo '```'
    echo
  fi

  if [ "$RENAME_HIT" = true ] && [ "$MATRIX_OK" = false ]; then
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

  if [ -n "$UI_SIGNAL" ] && [ "$MATRIX_OK" = false ]; then
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
    echo "Each row must be a full matrix row -- running slot, target slot, image SHA-256, boot result and a \`PASS\` / \`FAIL\` / \`N/A\` verdict (\`N/A\` only for ${NA_PERMITTED_CASES[*]}). Write \`-\` where a field genuinely does not apply:"
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

  if [ "$MATRIX_OK" = false ]; then
    echo "### What this gate will NOT accept as a substitute"
    echo
    echo "- A green build on all four board environments"
    echo "- A passing host test suite, however large"
    echo "- \`cppcheck\`, CodeQL, or a clean CodeRabbit review"
    echo "- A single successful installation on real hardware"
    echo
    echo "All of those passed on the change that bricked the device. None of them runs on hardware AND exercises an install *cycle*. Only a designated test unit completing the two-slot matrix does."
    echo
  fi

  echo "### How to unblock"
  echo
  if [ "$MATRIX_OK" = false ]; then
    echo "Firmware update path:"
    echo
    echo "1. Read **\`$GATE_DOC\`** and run the matrix on a designated test device -- never the only working unit."
    echo "2. Record running slot, target slot, image SHA-256 and boot result for every transition, per channel (SD and OTA have different erase expectations -- see §4)."
    echo "3. Paste the filled-in results block from §5 of that document into this PR's description, or add the report under \`docs/hardware-validation/\`."
    echo
  fi
  if [ "$SELF_MOD_OK" = false ]; then
    echo "Gate self-modification:"
    echo
    echo "1. Have a maintainer (${MAINTAINER_ASSOCIATIONS[*]}) review the change to the gate's own files and submit an **Approve** review."
    echo "2. That review must be recorded against the current head SHA \`$HEAD_SHA\`. If you push again afterwards, it must be re-approved."
    echo
  fi
  echo "Gate definition: \`$GATE_DOC\`"
}
exit 1
