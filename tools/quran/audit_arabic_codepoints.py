#!/usr/bin/env python3
"""Corpus-driven audit of lib/ArabicShaper/ArabicCharacter.cpp's codepoint
classification against all 114 KFGQPC Uthmani chapter source files.

Scans every codepoint actually present in the Arabic Unicode blocks across the
corpus, reimplements ArabicCharacter.cpp's isArabicDiacritic/getJoiningType logic
in Python (kept in exact sync with the C++ below), and cross-checks each against
two independent ground-truth sources: unicodedata.combining() (Unicode's own
canonical combining class, for the diacritic/transparent question) and
arabic_reshaper's LETTERS_ARABIC presentation-forms table (for the joining
direction question, derived from real isolated/initial/medial/final glyph
availability rather than a hand-maintained list).

Usage: python3 audit_arabic_codepoints.py
"""
import glob
import os
import re
import sys
import unicodedata

try:
    from arabic_reshaper import letters as reshaper_letters
except ImportError:
    print("ERROR: pip install arabic-reshaper", file=sys.stderr)
    sys.exit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CHAPTERS_GLOB = os.path.join(SCRIPT_DIR, "source", "kfgqpc_chapters", "chapter-*.xhtml")

ARABIC_BLOCKS = [
    (0x0600, 0x06FF),
    (0xFB50, 0xFDFF),
    (0xFE70, 0xFEFF),
]


def in_arabic_block(cp):
    return any(lo <= cp <= hi for lo, hi in ARABIC_BLOCKS)


# ---- Python mirror of lib/ArabicShaper/ArabicCharacter.cpp -- keep in sync ----

def is_arabic_diacritic(cp):
    return (
        (0x064B <= cp <= 0x065F)
        or cp == 0x0670
        or (0x0610 <= cp <= 0x061A)
        or (0x06D6 <= cp <= 0x06DC)
        or (0x06DF <= cp <= 0x06E4)
        or (0x06E7 <= cp <= 0x06E8)
        or (0x06EA <= cp <= 0x06ED)
    )


_EXTENDED_ARABIC_LETTERS = {
    0x067E, 0x0686, 0x0698, 0x06A9, 0x06AD, 0x06AF, 0x06C0, 0x06CC, 0x06D5,
}


def is_extended_arabic_letter(cp):
    return cp in _EXTENDED_ARABIC_LETTERS


def is_arabic_base_char(cp):
    return (0x0621 <= cp <= 0x064A and not is_arabic_diacritic(cp)) or is_extended_arabic_letter(cp)


_RIGHT_JOINING_ONLY = {
    0x0622, 0x0623, 0x0624, 0x0625, 0x0627, 0x0629, 0x062F, 0x0630, 0x0631,
    0x0632, 0x0648, 0x0671, 0x0698, 0x06C0, 0x06D5,
    0xFEF5, 0xFEF6, 0xFEF7, 0xFEF8, 0xFEF9, 0xFEFA, 0xFEFB, 0xFEFC,
}


def get_joining_type(cp):
    if is_arabic_diacritic(cp):
        return "TRANSPARENT"
    if cp in _RIGHT_JOINING_ONLY:
        return "RIGHT_JOINING"
    if is_arabic_base_char(cp) and cp != 0x0621:
        return "DUAL_JOINING"
    return "NON_JOINING"


# ---- Ground truth from arabic_reshaper + unicodedata ----

def reshaper_joining_type(ch):
    """None if arabic_reshaper has no shaping data for this letter (out of scope
    for this cross-check -- e.g. digits, punctuation, marks); else one of the
    same 4 category names our classifier uses."""
    forms = reshaper_letters.LETTERS_ARABIC.get(ch)
    if forms is None:
        return None
    connects_after = bool(forms[reshaper_letters.INITIAL] or forms[reshaper_letters.MEDIAL])
    connects_before = bool(forms[reshaper_letters.FINAL] or forms[reshaper_letters.MEDIAL])
    if connects_after and connects_before:
        return "DUAL_JOINING"
    if connects_before:
        return "RIGHT_JOINING"
    if connects_after:
        return "LEFT_JOINING"
    return "NON_JOINING"


def scan_corpus_codepoints():
    files = sorted(glob.glob(CHAPTERS_GLOB))
    if not files:
        print(f"ERROR: no chapter files found at {CHAPTERS_GLOB}", file=sys.stderr)
        sys.exit(1)
    codepoints = {}  # cp -> set of chapter basenames it appears in (capped)
    for path in files:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        text = re.sub(r"<[^>]+>", "", text)  # strip tags, keep only text content
        for ch in text:
            cp = ord(ch)
            if in_arabic_block(cp):
                bucket = codepoints.setdefault(cp, set())
                if len(bucket) < 3:
                    bucket.add(os.path.basename(path))
    return len(files), codepoints


def main():
    n_files, codepoints = scan_corpus_codepoints()
    print(f"Scanned {n_files} chapter files, found {len(codepoints)} distinct Arabic-block codepoints.\n")

    mismatches = []
    informational = []

    for cp in sorted(codepoints):
        ch = chr(cp)
        name = unicodedata.name(ch, "UNNAMED")
        ccc = unicodedata.combining(ch)
        cat = unicodedata.category(ch)
        ours_diacritic = is_arabic_diacritic(cp)
        ours_joining = get_joining_type(cp)

        # Ground truth #1: combining-mark question, via unicodedata.
        is_mark_by_unicode = cat.startswith("M") or ccc != 0
        if is_mark_by_unicode != ours_diacritic:
            mismatches.append(
                (cp, name, f"isArabicDiacritic()={ours_diacritic} but unicodedata says "
                           f"category={cat} combining={ccc} (mark={is_mark_by_unicode})")
            )
            continue  # joining-type check below is meaningless if the diacritic call is wrong

        if ours_diacritic:
            continue  # transparent marks have no joining-direction ground truth to check

        # Ground truth #2: joining-direction question, via arabic_reshaper's real
        # presentation-form availability (only meaningful for base Arabic letters).
        truth = reshaper_joining_type(ch)
        if truth is None:
            informational.append((cp, name, ours_joining, "no arabic_reshaper data (digit/punct/other)"))
            continue
        if truth != ours_joining:
            mismatches.append(
                (cp, name, f"getJoiningType()={ours_joining} but arabic_reshaper shaping "
                           f"data implies {truth}")
            )

    print(f"=== {len(mismatches)} MISMATCH(ES) ===")
    for cp, name, detail in mismatches:
        chapters = ", ".join(sorted(codepoints[cp]))
        print(f"  U+{cp:04X} {name}: {detail}  [seen in: {chapters}]")

    print(f"\n=== {len(informational)} informational (no ground truth available, not a bug) ===")
    for cp, name, ours_joining, note in informational:
        print(f"  U+{cp:04X} {name}: ours={ours_joining} -- {note}")

    print(f"\n{'PASS' if not mismatches else 'FAIL'}: {len(mismatches)} mismatch(es) against ground truth.")
    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
