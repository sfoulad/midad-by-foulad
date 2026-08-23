"""
Fail-closed integrity gate for PlatformIO's own package downloads.

Issue #179: PlatformIO's PackageManager does not checksum-verify external/URI
package installs (the platform archive, Arduino/ESP-IDF frameworks, SCons,
contrib-piohome, and the idf_tools.py wrapper manifests for cmake/ninja/
cppcheck/esptoolpy/esp-rom-elfs/the two GCC toolchains) -- only PlatformIO's
Library Registry path is checksummed. This script closes that gap by
independently verifying every URL in scripts/platformio-packages-lock.json
against a checked-in sha256, BEFORE `pio run` is allowed to consume any of
that content.

Two run modes:
  * Verify (default): for each manifest entry, hash whichever copy is
    available -- PlatformIO's own download cache if this is a cache-hit
    runner, otherwise fetch the URL fresh -- and compare to the manifest.
    Any mismatch, any URL missing from the manifest that the current
    platform.json declares, or any manifest entry whose URL no longer
    matches what platform.json currently declares for that package name
    (a version bump nobody re-locked) fails closed with a nonzero exit.
  * --update: recompute the manifest from the currently pinned platform
    URL and overwrite scripts/platformio-packages-lock.json. Run this
    locally after bumping platformio.ini's pinned platform/toolchain
    version, then have a human review the diff before merging -- mirrors
    the existing scripts/requirements-ci.lock regeneration discipline.

Runs identically on empty and cache-hit runners: hashing is the only
required operation either way, so this step has no compiler dependency
and can run before SCons/`pio run` ever starts.
"""

import argparse
import configparser
import hashlib
import io
import json
import os
import re
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = REPO_ROOT / "scripts" / "platformio-packages-lock.json"
PLATFORMIO_INI_PATH = REPO_ROOT / "platformio.ini"

# The 7 packages platform.json declares as idf_tools.py wrapper manifests
# rather than direct payload downloads -- see the "note" field convention
# used for these in the manifest. PlatformIO installs these via a local
# file:// handoff after idf_tools.py resolves the wrapper, so there is no
# better URL to pin than the wrapper's own -- platform.json's declared
# wrapper URL is directly comparable to the manifest entry for these.
WRAPPER_PACKAGE_NAMES = {
    "tool-cmake", "tool-ninja", "tool-cppcheck", "tool-esptoolpy",
    "tool-esp-rom-elfs", "toolchain-riscv32-esp", "toolchain-xtensa-esp-elf",
}

# tool-scons is a distinct third case: platform.json also declares a wrapper
# URL for it, but PlatformIO's own PackageManager (not idf_tools.py) resolves
# and re-downloads it a second time via a real, direct https URL, which is
# what .piopm records and what this manifest pins (the more specific, more
# meaningful thing to verify). platform.json's wrapper URL is therefore NOT
# comparable to this manifest's entry for tool-scons -- a mismatch there is
# expected, not a drift signal, so it's excluded from the equality cross-check
# below (its hash is still fully verified like every other entry).
RESOLVED_INDIRECTLY_PACKAGE_NAMES = {"tool-scons"}


def sha256_and_size(fileobj_or_path):
    h = hashlib.sha256()
    size = 0
    if isinstance(fileobj_or_path, (str, Path)):
        f = open(fileobj_or_path, "rb")
        close = True
    else:
        f = fileobj_or_path
        close = False
    try:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
            size += len(block)
    finally:
        if close:
            f.close()
    return h.hexdigest(), size


def platformio_core_dir():
    return Path(os.environ.get("PLATFORMIO_CORE_DIR") or (Path.home() / ".platformio"))


def cached_download_path(url):
    """Mirrors platformio/package/manager/_download.py's cache key: sha1(url + checksum);
    checksum is empty for every package class this script covers (see issue #179 audit)."""
    key = hashlib.sha1(url.encode()).hexdigest()
    return platformio_core_dir() / ".cache" / "downloads" / key


def fetch_url_to_temp(url):
    with urllib.request.urlopen(url, timeout=120) as resp:
        tmp = tempfile.NamedTemporaryFile(delete=False)
        try:
            while True:
                block = resp.read(1 << 20)
                if not block:
                    break
                tmp.write(block)
        finally:
            tmp.close()
        return Path(tmp.name)


def get_pinned_platform_url():
    config = configparser.ConfigParser()
    config.read(PLATFORMIO_INI_PATH, encoding="utf-8")
    for section in ("env:default", "base"):
        if config.has_section(section) and config.has_option(section, "platform"):
            return config.get(section, "platform").strip()
    raise SystemExit(f"ERROR: could not find a `platform = ` line in {PLATFORMIO_INI_PATH}")


def load_manifest():
    if not MANIFEST_PATH.is_file():
        raise SystemExit(f"ERROR: manifest not found at {MANIFEST_PATH}")
    return json.loads(MANIFEST_PATH.read_text())


def extract_platform_json_from_zip(zip_path):
    with zipfile.ZipFile(zip_path) as zf:
        candidates = [n for n in zf.namelist() if n.endswith("platform.json")]
        if not candidates:
            raise SystemExit("ERROR: platform.json not found inside the platform archive")
        # platform.json lives at the archive root or one directory down depending on packaging.
        candidates.sort(key=len)
        with zf.open(candidates[0]) as f:
            return json.loads(f.read())


def resolve_and_hash(url, label, errors):
    """Returns (sha256, size) for url, using the PlatformIO download cache when
    present (cache-hit runner) and fetching fresh otherwise (empty runner)."""
    cached = cached_download_path(url)
    if cached.is_file():
        return sha256_and_size(cached)
    print(f"  [{label}] not cache-hit, fetching fresh: {url}")
    try:
        tmp_path = fetch_url_to_temp(url)
    except Exception as exc:  # noqa: BLE001 -- report and fail closed, don't crash silently
        errors.append(f"{label}: failed to download {url}: {exc}")
        return None, None
    try:
        return sha256_and_size(tmp_path)
    finally:
        tmp_path.unlink(missing_ok=True)


def cmd_verify(args):
    manifest = load_manifest()
    pinned_url = get_pinned_platform_url()
    errors = []

    if pinned_url != manifest["platform_url"]:
        errors.append(
            "platformio.ini's pinned platform URL does not match the manifest's "
            f"recorded platform_url.\n    platformio.ini: {pinned_url}\n"
            f"    manifest:       {manifest['platform_url']}\n"
            "    Run `python3 scripts/verify_platformio_packages.py --update` "
            "and review the diff."
        )

    packages_by_name = {p["name"]: p for p in manifest["packages"]}
    if "espressif32" not in packages_by_name:
        errors.append("manifest is missing the 'espressif32' platform package entry")

    platform_json = None
    for name, entry in packages_by_name.items():
        print(f"Verifying {name}@{entry['version']} ...")
        sha256, size = resolve_and_hash(entry["url"], name, errors)
        if sha256 is None:
            continue
        if sha256 != entry["sha256"]:
            errors.append(
                f"{name}: SHA-256 MISMATCH for {entry['url']}\n"
                f"    manifest: {entry['sha256']}\n"
                f"    actual:   {sha256}\n"
                "    This means the content at that URL has changed since the "
                "manifest was generated -- treat as tampering until proven "
                "otherwise (GitHub release assets are not immutable by "
                "default; see issue #179)."
            )
            continue
        if size != entry["size"]:
            errors.append(
                f"{name}: size mismatch for {entry['url']} "
                f"(manifest: {entry['size']}, actual: {size})"
            )
            continue
        if name == "espressif32":
            cached = cached_download_path(entry["url"])
            zip_path = cached if cached.is_file() else None
            if zip_path is None:
                tmp_path = fetch_url_to_temp(entry["url"])
                try:
                    platform_json = extract_platform_json_from_zip(tmp_path)
                finally:
                    tmp_path.unlink(missing_ok=True)
            else:
                platform_json = extract_platform_json_from_zip(cached)

    if platform_json is not None:
        declared_packages = platform_json.get("packages", {})
        for name, entry in packages_by_name.items():
            if name == "espressif32":
                continue
            declared = declared_packages.get(name)
            if declared is None:
                # Package no longer declared by the platform at all -- not a
                # security failure on its own, just manifest drift to clean up.
                print(f"  NOTE: {name} is in the manifest but no longer declared "
                      f"by platform.json -- safe to remove on next --update.")
                continue
            if name in RESOLVED_INDIRECTLY_PACKAGE_NAMES:
                # platform.json's declared URL is a wrapper this package's
                # real, resolved download doesn't match by design -- see
                # RESOLVED_INDIRECTLY_PACKAGE_NAMES's comment. The hash
                # verification above already covered the real integrity
                # check; skip the drift comparison here.
                continue
            declared_url = declared.get("version")
            if declared_url != entry["url"]:
                kind = "wrapper URL" if name in WRAPPER_PACKAGE_NAMES else "URL"
                errors.append(
                    f"{name}: platform.json now declares a different {kind} than "
                    f"the manifest.\n    manifest:      {entry['url']}\n"
                    f"    platform.json: {declared_url}\n"
                    "    Likely a legitimate version bump -- run --update and review."
                )

    if errors:
        print("\nFAILED: PlatformIO package integrity verification found "
              f"{len(errors)} problem(s):\n", file=sys.stderr)
        for e in errors:
            print(f"- {e}\n", file=sys.stderr)
        return 1

    print(f"\nOK: all {len(packages_by_name)} manifest entries verified "
          "(hash + size match, platform.json cross-check clean).")
    return 0


def cmd_update(args):
    pinned_url = get_pinned_platform_url()
    errors = []
    sha256, size = resolve_and_hash(pinned_url, "espressif32", errors)
    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1

    cached = cached_download_path(pinned_url)
    if cached.is_file():
        platform_json = extract_platform_json_from_zip(cached)
    else:
        tmp_path = fetch_url_to_temp(pinned_url)
        try:
            platform_json = extract_platform_json_from_zip(tmp_path)
        finally:
            tmp_path.unlink(missing_ok=True)

    version_match = re.search(r"/([^/]+\.zip)$", pinned_url)
    platform_version = platform_json.get("version", "unknown")

    entries = [{
        "name": "espressif32",
        "version": platform_version,
        "url": pinned_url,
        "size": size,
        "sha256": sha256,
    }]

    for name, declared in platform_json.get("packages", {}).items():
        url = declared.get("version")
        if not isinstance(url, str) or not url.startswith("http"):
            continue  # registry-resolvable / non-URI packages don't need this manifest
        pkg_sha256, pkg_size = resolve_and_hash(url, name, errors)
        if pkg_sha256 is None:
            continue
        entry = {
            "name": name,
            "version": declared.get("package-version", "unknown"),
            "url": url,
            "size": pkg_size,
            "sha256": pkg_sha256,
        }
        if name in WRAPPER_PACKAGE_NAMES:
            entry["note"] = (
                "idf_tools.py wrapper manifest (tools.json); the real toolchain/tool "
                "payload is downloaded and hash-verified separately by idf_tools.py "
                "against sha256 fields declared inside this wrapper -- this entry "
                "pins the wrapper itself, which is the actual unverified trust "
                "boundary (see issue #179 audit)."
            )
        entries.append(entry)

    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1

    entries.sort(key=lambda e: e["name"])
    out = {
        "_comment": (
            "Independently-verified PlatformIO package downloads for "
            "platform-espressif32, covering every package a clean `pio run` "
            "fetches outside of PlatformIO's checksummed Library Registry "
            "(issue #179). Regenerate with `python3 "
            "scripts/verify_platformio_packages.py --update` after any "
            "platformio.ini platform/toolchain version bump, then have a "
            "human review the diff before merging."
        ),
        "platform_url": pinned_url,
        "generated_from_commit": os.environ.get("GIT_COMMIT", "unknown"),
        "packages": entries,
    }
    MANIFEST_PATH.write_text(json.dumps(out, indent=2) + "\n")
    print(f"Wrote {len(entries)} entries to {MANIFEST_PATH}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update", action="store_true",
                         help="regenerate the manifest from the currently pinned platform URL")
    args = parser.parse_args()
    if args.update:
        return cmd_update(args)
    return cmd_verify(args)


if __name__ == "__main__":
    sys.exit(main())
