#!/usr/bin/env bash
# classify-crosspoint-sync.sh <base-ref> <head-ref> [sensitive-paths-file]
#
# Stage 0 of the CrossPoint auto-sync design (docs/crosspoint-auto-sync-design.md,
# §3-4): classifies an ALREADY-CLEAN CrossPoint merge as:
#
#   A -- clean, zero sensitive-path hits (future auto-merge eligible; this
#        script never merges or auto-merges anything itself)
#   B -- clean, but touches at least one sensitive category (human review
#        required)
#
# Class C (merge conflict) is decided by the caller BEFORE this script ever
# runs -- a conflicted merge produces no diff to classify, so this script
# assumes <base-ref>..<head-ref> is a real, already-clean merge (or any
# other diff the caller wants classified the same way).
#
# <base-ref> is Midad's tip before the merge (develop); <head-ref> is the
# sync branch after merging upstream in -- so the diff between them is
# exactly the set of files the merge touched.
#
# Renames are read via `git diff --name-status -M`, which reports a rename
# as `R<score>\told_path\tnew_path` -- BOTH paths are checked against the
# sensitive-path policy, so a rename that moves a file INTO or OUT OF a
# sensitive category is caught either direction. Same reasoning as
# scripts/thin-fork-guard.sh's own rename handling.
#
# Fails closed: a missing/empty sensitive-path config, or any failure to
# read the diff, aborts with a non-zero exit and prints why -- never
# silently defaults to Class A.
#
# Output: the full report on stdout (redirect to $GITHUB_STEP_SUMMARY), and
# the single line `CLASS=A` or `CLASS=B` as the LAST line of stdout, so a
# caller can extract it with `tail -1` or `grep -E '^CLASS='`.
set -euo pipefail

BASE_REF="${1:?usage: classify-crosspoint-sync.sh <base-ref> <head-ref> [sensitive-paths-file]}"
HEAD_REF="${2:?usage: classify-crosspoint-sync.sh <base-ref> <head-ref> [sensitive-paths-file]}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SENSITIVE_PATHS_FILE="${3:-$SCRIPT_DIR/../.github/crosspoint-sensitive-paths.yml}"

if [ ! -f "$SENSITIVE_PATHS_FILE" ]; then
  echo "classify-crosspoint-sync.sh: sensitive-path config not found at '$SENSITIVE_PATHS_FILE' -- failing closed" >&2
  exit 1
fi

# --- Load "category: [ - \"glob\", ... ]" pairs from the YAML config --------
# A small, deliberately naive line scanner, not a real YAML parser -- this
# file's shape is fixed by this script and reviewed like any other repo
# file, not attacker-authored free-form YAML (contrast
# scripts/check-workflow-permissions.rb, which parses workflow YAML that
# arrives via an untrusted PR and therefore does use a real parser).
CATEGORY_PATTERN_PAIRS=()
current_category=""
while IFS= read -r line; do
  if [[ "$line" =~ ^([a-z_]+):[[:space:]]*$ ]]; then
    current_category="${BASH_REMATCH[1]}"
    continue
  fi
  if [[ "$line" =~ ^[[:space:]]*-[[:space:]]+\"(.*)\"[[:space:]]*$ ]]; then
    [ -z "$current_category" ] && continue
    CATEGORY_PATTERN_PAIRS+=("${current_category}|||${BASH_REMATCH[1]}")
  fi
done <"$SENSITIVE_PATHS_FILE"

# Two rules hard-coded here, not overridable by editing the YAML -- see
# docs/crosspoint-auto-sync-design.md §4 for why these two are pinned in
# code: `.github/**` covers "upstream modifying our workflows" and "changes
# to the auto-sync workflow itself"; `freeink-sdk` covers a gitlink-pointer
# change, which content-diff path globbing can't meaningfully distinguish
# from an ordinary file the same way.
CATEGORY_PATTERN_PAIRS+=("hardcoded|||.github/**" "hardcoded|||freeink-sdk")

if [ "${#CATEGORY_PATTERN_PAIRS[@]}" -eq 0 ]; then
  echo "classify-crosspoint-sync.sh: sensitive-path config at '$SENSITIVE_PATHS_FILE' contained zero patterns -- failing closed (an empty policy must never silently mean 'nothing is sensitive')" >&2
  exit 1
fi

# Echoes the matched category on the first match, or returns 1 if none match.
match_category() {
  local path="$1" pair category pattern
  for pair in "${CATEGORY_PATTERN_PAIRS[@]}"; do
    category="${pair%%|||*}"
    pattern="${pair#*|||}"
    # Deliberate glob match against a string, not a literal comparison --
    # bash's [[ == ]] pattern matching already treats `*` as matching `/`
    # here (this is not filesystem globbing), so `**` and `*` are
    # equivalent; `**` is used in the config purely for human readability.
    # shellcheck disable=SC2053 # deliberate unquoted glob, see comment above
    if [[ "$path" == $pattern ]]; then
      echo "$category"
      return 0
    fi
  done
  return 1
}

# --- Diff surface (renames included, both paths checked) --------------------
if ! DIFF_OUTPUT="$(git diff --name-status -M "$BASE_REF" "$HEAD_REF" 2>&1)"; then
  echo "classify-crosspoint-sync.sh: 'git diff --name-status -M $BASE_REF $HEAD_REF' failed -- failing closed: $DIFF_OUTPUT" >&2
  exit 1
fi

TOUCHED_PATHS=()
while IFS=$'\t' read -r status path1 path2; do
  [ -z "$status" ] && continue
  case "$status" in
    R*)
      TOUCHED_PATHS+=("$path1" "$path2")
      ;;
    *)
      TOUCHED_PATHS+=("$path1")
      ;;
  esac
done <<<"$DIFF_OUTPUT"

# path|||category, one per sensitive hit (a path may only match its first
# hit -- match_category returns on first match -- which is fine, this is a
# classification signal, not an exhaustive audit of every category a path
# could theoretically belong to).
SENSITIVE_HITS=()
# The `${ARR[@]+"${ARR[@]}"}` idiom (rather than a bare "${TOUCHED_PATHS[@]}")
# avoids bash's pre-4.4 "unbound variable" behavior under `set -u` when the
# array has zero elements (e.g. an empty merge diff) -- macOS still ships
# bash 3.2 by default, so this matters for local runs, not just CI.
for path in "${TOUCHED_PATHS[@]+"${TOUCHED_PATHS[@]}"}"; do
  [ -z "$path" ] && continue
  if category="$(match_category "$path")"; then
    SENSITIVE_HITS+=("${path}|||${category}")
  fi
done

# --- Report -------------------------------------------------------------------
echo "## CrossPoint Sync Classifier"
echo
echo "Base: \`$BASE_REF\`  Head: \`$HEAD_REF\`"
echo "Files touched by this merge: **${#TOUCHED_PATHS[@]}**"
echo

if [ "${#TOUCHED_PATHS[@]}" -eq 0 ]; then
  echo "No files touched -- nothing to classify (identical trees)."
fi

if [ "${#SENSITIVE_HITS[@]}" -gt 0 ]; then
  HIT_CATEGORIES=$(printf '%s\n' "${SENSITIVE_HITS[@]}" | awk -F'\\|\\|\\|' '{print $2}' | sort -u)
  echo "### Sensitive categories touched"
  echo '```'
  echo "$HIT_CATEGORIES"
  echo '```'
  echo
  echo "### Sensitive-path hits (${#SENSITIVE_HITS[@]})"
  echo '```'
  printf '%s\n' "${SENSITIVE_HITS[@]}" | awk -F'\\|\\|\\|' '{print $1 "  [" $2 "]"}' | sort -u
  echo '```'
  echo
else
  echo "No sensitive-path hits."
  echo
fi

if [ "${#SENSITIVE_HITS[@]}" -gt 0 ]; then
  categories_csv=$(printf '%s\n' "${SENSITIVE_HITS[@]}" | awk -F'\\|\\|\\|' '{print $2}' | sort -u | paste -sd, -)
  echo "CLASS: B -- clean merge, human review required"
  echo "CATEGORIES=$categories_csv"
  echo "CLASS=B"
else
  echo "CLASS: A -- clean merge, future auto-merge eligible (auto-merge currently disabled)"
  echo "CATEGORIES="
  echo "CLASS=A"
fi
