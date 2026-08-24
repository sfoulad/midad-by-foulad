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
import json
import os
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
    # Confirmed the same wrapper shape (package.json + tools.json, real
    # payload resolved by idf_tools.py) by downloading and inspecting each
    # of these directly during PR review -- see issue #179 Round 1.
    "tool-clangtidy", "tool-dfuutil-arduino", "tool-openocd-esp32",
    "tool-pvs-studio", "tool-riscv32-esp-elf-gdb", "tool-xtensa-esp-elf-gdb",
    "toolchain-esp32ulp",
}

# tool-scons is a distinct third case: platform.json also declares a wrapper
# URL for it, but PlatformIO's own PackageManager (not idf_tools.py) resolves
# and re-downloads it a second time via a real, direct https URL, which is
# what .piopm records and what this manifest pins (the more specific, more
# meaningful thing to verify). platform.json's wrapper URL is therefore never
# directly comparable to entry["url"] for these -- but the wrapper URL still
# needs its own drift check (a wrapper bump is exactly what --update would
# follow next), so cmd_update additionally records it as entry["wrapper_url"],
# and the cross-check compares platform.json against that field instead.
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


def fetch_url_to_cache(url, label, errors):
    """Downloads url directly into PlatformIO's own cache location (atomic
    rename), so a verified download is the exact same file PlatformIO's own
    downloader will later find and reuse -- otherwise a cache-miss run would
    verify one copy, discard it, and let `pio run` fetch and build from an
    unverified second copy that isn't guaranteed to be identical."""
    target = cached_download_path(url)
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        with urllib.request.urlopen(url, timeout=120) as resp:
            fd, tmp_name = tempfile.mkstemp(dir=str(target.parent))
            try:
                with os.fdopen(fd, "wb") as tmp:
                    while True:
                        block = resp.read(1 << 20)
                        if not block:
                            break
                        tmp.write(block)
                os.replace(tmp_name, target)  # atomic within the same filesystem
            except Exception:
                Path(tmp_name).unlink(missing_ok=True)
                raise
    except Exception as exc:  # noqa: BLE001 -- report and fail closed, don't crash silently
        errors.append(f"{label}: failed to download {url}: {exc}")
        return None
    return target


def discard_cached_download(url):
    """Deletes a cache-miss download that failed verification, so a
    tampered/corrupt artifact is never left where a later `pio run` could
    pick it up as if it were the verified copy."""
    cached_download_path(url).unlink(missing_ok=True)


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
    present (cache-hit runner) and fetching fresh otherwise (empty runner).
    A fresh fetch is stored directly at PlatformIO's own cache path (see
    fetch_url_to_cache) -- the caller must call discard_cached_download(url)
    if the returned hash/size doesn't match what was expected, so a bad
    download never lingers where `pio run` could reuse it unverified."""
    cached = cached_download_path(url)
    if cached.is_file():
        return sha256_and_size(cached)
    print(f"  [{label}] not cache-hit, fetching fresh: {url}")
    cached = fetch_url_to_cache(url, label, errors)
    if cached is None:
        return None, None
    return sha256_and_size(cached)


def cross_check_manifest_against_platform_json(packages_by_name, platform_json):
    """Returns a list of error strings (empty if clean). Two directions:
    manifest entries whose pinned URL no longer matches what platform.json
    currently declares (a version bump nobody re-locked), and -- the actual
    coverage guarantee -- packages platform.json declares that the manifest
    has never heard of at all, which would reach `pio run` completely
    unverified."""
    errors = []
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
            # The manifest pins the resolved artifact, not the wrapper --
            # but the wrapper URL still needs its OWN drift check, or a
            # wrapper bump (which idf_tools.py-style resolution would follow
            # on the next real `pio run`) would go completely undetected
            # here. Compare against the recorded wrapper_url instead of the
            # resolved artifact's own url. (The wrapper's own bytes are
            # separately hash-verified in cmd_verify's main loop -- a URL
            # match alone doesn't prove the content behind it is unchanged,
            # since GitHub release assets aren't immutable by default; see
            # issue #179.)
            wrapper_url = entry.get("wrapper_url")
            missing = [f for f in ("wrapper_url", "wrapper_sha256", "wrapper_size") if not entry.get(f)]
            if missing:
                errors.append(
                    f"{name}: manifest entry is missing {', '.join(missing)}, so "
                    "the wrapper's URL and content can't be verified for drift/"
                    "tampering. Run --update."
                )
            elif declared.get("version") != wrapper_url:
                errors.append(
                    f"{name}: platform.json now declares a different wrapper URL "
                    f"than the manifest.\n    manifest:      {wrapper_url}\n"
                    f"    platform.json: {declared.get('version')}\n"
                    "    Likely a legitimate version bump -- run --update and review."
                )
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

    for name, declared in declared_packages.items():
        url = declared.get("version")
        if not isinstance(url, str) or not url.startswith("http"):
            continue  # registry-resolvable spec, not a URI install -- out of this manifest's scope
        if name not in packages_by_name:
            errors.append(
                f"{name}: platform.json declares this package (url: {url}) but "
                "the manifest has no entry for it at all -- its content would "
                "reach `pio run` completely unverified. Run --update and review."
            )

    return errors


def verify_wrapper_hash(name, entry):
    """For RESOLVED_INDIRECTLY_PACKAGE_NAMES entries: the resolved artifact's
    hash is verified by cmd_verify's main loop, but that alone doesn't prove
    the wrapper it came from is unchanged -- the URL cross-check only
    compares the wrapper *URL*, and GitHub release assets aren't immutable
    by default (issue #179), so the same URL can serve different bytes over
    time. Independently hash-verifies the wrapper itself. Returns a list of
    error strings (empty if clean). Fails closed -- not silently skips -- if
    any of wrapper_url/wrapper_sha256/wrapper_size is missing (an older or
    hand-edited manifest entry): this function runs independently of
    cross_check_manifest_against_platform_json's own missing-field check, so
    it must not rely on that other call site to catch the same gap."""
    errors = []
    wrapper_url = entry.get("wrapper_url")
    wrapper_sha256_expected = entry.get("wrapper_sha256")
    wrapper_size_expected = entry.get("wrapper_size")
    missing = [f for f in ("wrapper_url", "wrapper_sha256", "wrapper_size") if not entry.get(f)]
    if missing:
        return [
            f"{name}: manifest entry is missing {', '.join(missing)}, so the "
            "wrapper's content can't be verified. Run --update."
        ]
    w_sha256, w_size = resolve_and_hash(wrapper_url, f"{name} (wrapper)", errors)
    if w_sha256 is None:
        return errors
    if w_sha256 != wrapper_sha256_expected:
        errors.append(
            f"{name}: SHA-256 MISMATCH for wrapper {wrapper_url}\n"
            f"    manifest: {wrapper_sha256_expected}\n"
            f"    actual:   {w_sha256}\n"
            "    The wrapper's content changed at an unchanged URL -- "
            "treat as tampering until proven otherwise."
        )
        discard_cached_download(wrapper_url)
    elif w_size != wrapper_size_expected:
        errors.append(
            f"{name}: size mismatch for wrapper {wrapper_url} "
            f"(manifest: {wrapper_size_expected}, actual: {w_size})"
        )
        discard_cached_download(wrapper_url)
    return errors


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
            discard_cached_download(entry["url"])
            continue
        if size != entry["size"]:
            errors.append(
                f"{name}: size mismatch for {entry['url']} "
                f"(manifest: {entry['size']}, actual: {size})"
            )
            discard_cached_download(entry["url"])
            continue
        if name == "espressif32":
            # resolve_and_hash has already ensured the verified bytes are
            # sitting at this exact path (cache-hit or freshly fetched) --
            # reuse that same file rather than issuing a second, separately
            # unverified download for platform.json extraction.
            platform_json = extract_platform_json_from_zip(cached_download_path(entry["url"]))

        if name in RESOLVED_INDIRECTLY_PACKAGE_NAMES:
            errors.extend(verify_wrapper_hash(name, entry))

    if platform_json is not None:
        errors.extend(cross_check_manifest_against_platform_json(packages_by_name, platform_json))

    if errors:
        print("\nFAILED: PlatformIO package integrity verification found "
              f"{len(errors)} problem(s):\n", file=sys.stderr)
        for e in errors:
            print(f"- {e}\n", file=sys.stderr)
        return 1

    print(f"\nOK: all {len(packages_by_name)} manifest entries verified "
          "(hash + size match, platform.json cross-check clean).")
    return 0


def resolve_wrapper_real_target(wrapper_url, name, errors):
    """For RESOLVED_INDIRECTLY_PACKAGE_NAMES: the wrapper zip embeds its own
    tools.json describing the real, final download (see tool-scons's own
    package.json/tools.json, inspected directly during issue #179 Round 1 --
    same shape as the idf_tools.py wrapper manifests, just resolved by
    PlatformIO's own PackageManager instead of idf_tools.py). Returns
    (url, sha256, size, wrapper_sha256, wrapper_size) -- the last two so the
    wrapper's own bytes can be pinned and re-verified later, since a wrapper
    URL match alone doesn't prove its content hasn't changed (GitHub release
    assets aren't immutable by default; see issue #179). Returns None if the
    real target can't be found -- callers must fail closed on None rather
    than falling back to pinning the wrapper URL itself, which would
    silently weaken this package's entry."""
    wrapper_sha256, wrapper_size = resolve_and_hash(wrapper_url, name, errors)
    if wrapper_sha256 is None:
        return None
    wrapper_path = cached_download_path(wrapper_url)
    try:
        with zipfile.ZipFile(wrapper_path) as zf:
            with zf.open("tools.json") as f:
                wrapper_tools_json = json.loads(f.read())
    except (KeyError, zipfile.BadZipFile, json.JSONDecodeError) as exc:
        errors.append(f"{name}: could not read tools.json from wrapper {wrapper_url}: {exc}")
        return None

    tools = wrapper_tools_json.get("tools", [])
    if len(tools) != 1 or not tools[0].get("versions"):
        errors.append(f"{name}: wrapper tools.json has unexpected shape (expected exactly "
                       f"one tool with versions) -- {wrapper_url}")
        return None
    versions = tools[0]["versions"]
    if len(versions) != 1:
        errors.append(f"{name}: wrapper tools.json declares {len(versions)} versions, "
                       f"expected exactly 1 -- {wrapper_url}")
        return None
    platform_targets = {k: v for k, v in versions[0].items() if isinstance(v, dict) and "url" in v}
    target = platform_targets.get("any")
    if target is None:
        errors.append(f"{name}: wrapper tools.json has no platform-independent ('any') "
                       f"target -- per-platform resolution isn't implemented, "
                       f"available keys: {sorted(platform_targets)}")
        return None
    return target["url"], target["sha256"], target["size"], wrapper_sha256, wrapper_size


def cmd_update(args):
    pinned_url = get_pinned_platform_url()
    errors = []
    sha256, size = resolve_and_hash(pinned_url, "espressif32", errors)
    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1

    # resolve_and_hash has already ensured the verified bytes are sitting at
    # this exact cache path -- reuse them rather than a second, separately
    # unverified download.
    platform_json = extract_platform_json_from_zip(cached_download_path(pinned_url))

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

        note = None
        wrapper_url = None
        wrapper_sha256 = None
        wrapper_size = None
        if name in RESOLVED_INDIRECTLY_PACKAGE_NAMES:
            wrapper_url = url
            resolved = resolve_wrapper_real_target(url, name, errors)
            if resolved is None:
                continue  # error already recorded; --update fails closed below rather
                          # than silently pinning the wrapper URL for this package
            url, pkg_sha256, pkg_size, wrapper_sha256, wrapper_size = resolved
        else:
            pkg_sha256, pkg_size = resolve_and_hash(url, name, errors)
            if pkg_sha256 is None:
                continue
            if name in WRAPPER_PACKAGE_NAMES:
                note = (
                    "idf_tools.py wrapper manifest (tools.json); the real toolchain/tool "
                    "payload is downloaded and hash-verified separately by idf_tools.py "
                    "against sha256 fields declared inside this wrapper -- this entry "
                    "pins the wrapper itself, which is the actual unverified trust "
                    "boundary (see issue #179 audit)."
                )

        entry = {
            "name": name,
            "version": declared.get("package-version", "unknown"),
            "url": url,
            "size": pkg_size,
            "sha256": pkg_sha256,
        }
        if wrapper_url:
            entry["wrapper_url"] = wrapper_url
            entry["wrapper_sha256"] = wrapper_sha256
            entry["wrapper_size"] = wrapper_size
        if note:
            entry["note"] = note
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
