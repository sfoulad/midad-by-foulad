"""Tests for scripts/verify_platformio_packages.py's core logic, using tiny
synthetic manifests/blobs -- not real multi-hundred-MB package downloads.
The hashing/comparison logic itself is what needs regression coverage; the
real end-to-end run (real manifest against a real clean PLATFORMIO_CORE_DIR)
was validated manually during issue #179 Round 1 groundwork."""

import hashlib
import importlib.util
import io
import json
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parent.parent / "verify_platformio_packages.py"
spec = importlib.util.spec_from_file_location("verify_platformio_packages", MODULE_PATH)
vpp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vpp)


class Sha256AndSizeTest(unittest.TestCase):
    def test_hashes_file_by_path(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "blob.bin"
            data = b"hello world" * 100
            path.write_bytes(data)
            sha256, size = vpp.sha256_and_size(path)
            self.assertEqual(sha256, hashlib.sha256(data).hexdigest())
            self.assertEqual(size, len(data))

    def test_hashes_file_object(self):
        data = b"some bytes to hash"
        sha256, size = vpp.sha256_and_size(io.BytesIO(data))
        self.assertEqual(sha256, hashlib.sha256(data).hexdigest())
        self.assertEqual(size, len(data))

    def test_empty_input(self):
        sha256, size = vpp.sha256_and_size(io.BytesIO(b""))
        self.assertEqual(sha256, hashlib.sha256(b"").hexdigest())
        self.assertEqual(size, 0)


class CachedDownloadPathTest(unittest.TestCase):
    def test_cache_key_matches_platformio_scheme(self):
        # Mirrors platformio/package/manager/_download.py's cache key:
        # sha1(url + checksum), checksum empty for every URI-install package
        # class this script covers (see issue #179 audit finding).
        url = "https://example.com/some-package-1.0.0.zip"
        expected_key = hashlib.sha1(url.encode()).hexdigest()
        path = vpp.cached_download_path(url)
        self.assertEqual(path.name, expected_key)
        self.assertEqual(path.parent.name, "downloads")
        self.assertEqual(path.parent.parent.name, ".cache")

    def test_respects_platformio_core_dir_env(self, ):
        import os
        old = os.environ.get("PLATFORMIO_CORE_DIR")
        try:
            os.environ["PLATFORMIO_CORE_DIR"] = "/tmp/fake-core-dir"
            path = vpp.cached_download_path("https://example.com/x.zip")
            self.assertTrue(str(path).startswith("/tmp/fake-core-dir"))
        finally:
            if old is None:
                os.environ.pop("PLATFORMIO_CORE_DIR", None)
            else:
                os.environ["PLATFORMIO_CORE_DIR"] = old


class WrapperPackageClassificationTest(unittest.TestCase):
    def test_known_wrapper_packages(self):
        for name in ("tool-cmake", "tool-ninja", "tool-cppcheck", "tool-esptoolpy",
                     "tool-esp-rom-elfs", "toolchain-riscv32-esp", "toolchain-xtensa-esp-elf",
                     "tool-clangtidy", "tool-dfuutil-arduino", "tool-openocd-esp32",
                     "tool-pvs-studio", "tool-riscv32-esp-elf-gdb", "tool-xtensa-esp-elf-gdb",
                     "toolchain-esp32ulp"):
            self.assertIn(name, vpp.WRAPPER_PACKAGE_NAMES)

    def test_tool_scons_is_resolved_indirectly_not_a_plain_wrapper(self):
        # Regression guard for the false-positive this distinction was added
        # to fix: tool-scons's manifest URL is the real resolved download
        # (matching .piopm), not platform.json's wrapper URL -- so it must
        # NOT be treated the same as the 7 file://-delegated packages.
        self.assertNotIn("tool-scons", vpp.WRAPPER_PACKAGE_NAMES)
        self.assertIn("tool-scons", vpp.RESOLVED_INDIRECTLY_PACKAGE_NAMES)


class ExtractPlatformJsonFromZipTest(unittest.TestCase):
    def test_extracts_platform_json_at_archive_root(self):
        import tempfile
        import zipfile
        with tempfile.TemporaryDirectory() as tmp:
            zip_path = Path(tmp) / "platform.zip"
            payload = {"name": "espressif32", "version": "1.2.3", "packages": {}}
            with zipfile.ZipFile(zip_path, "w") as zf:
                zf.writestr("platform.json", json.dumps(payload))
                zf.writestr("builder/main.py", "# unrelated file")
            result = vpp.extract_platform_json_from_zip(zip_path)
            self.assertEqual(result, payload)

    def test_extracts_platform_json_nested_one_level(self):
        import tempfile
        import zipfile
        with tempfile.TemporaryDirectory() as tmp:
            zip_path = Path(tmp) / "platform.zip"
            payload = {"name": "espressif32", "version": "1.2.3", "packages": {}}
            with zipfile.ZipFile(zip_path, "w") as zf:
                zf.writestr("platform-espressif32-55.3.37/platform.json", json.dumps(payload))
            result = vpp.extract_platform_json_from_zip(zip_path)
            self.assertEqual(result, payload)

    def test_missing_platform_json_raises(self):
        import tempfile
        import zipfile
        with tempfile.TemporaryDirectory() as tmp:
            zip_path = Path(tmp) / "platform.zip"
            with zipfile.ZipFile(zip_path, "w") as zf:
                zf.writestr("readme.txt", "no platform.json in here")
            with self.assertRaises(SystemExit):
                vpp.extract_platform_json_from_zip(zip_path)


class CrossCheckManifestAgainstPlatformJsonTest(unittest.TestCase):
    def test_clean_manifest_matching_platform_json_has_no_errors(self):
        packages_by_name = {
            "espressif32": {"url": "https://example.com/platform.zip"},
            "tool-foo": {"url": "https://example.com/foo-1.0.zip"},
        }
        platform_json = {"packages": {
            "tool-foo": {"version": "https://example.com/foo-1.0.zip"},
        }}
        self.assertEqual(vpp.cross_check_manifest_against_platform_json(packages_by_name, platform_json), [])

    def test_package_declared_by_platform_json_but_missing_from_manifest_is_an_error(self):
        # Regression test for the coverage gap CodeRabbit found in this PR's
        # review: platform.json declaring a package the manifest never
        # covers must fail closed, not be silently skipped.
        packages_by_name = {
            "espressif32": {"url": "https://example.com/platform.zip"},
        }
        platform_json = {"packages": {
            "tool-never-locked": {"version": "https://example.com/never-locked-1.0.zip"},
        }}
        errors = vpp.cross_check_manifest_against_platform_json(packages_by_name, platform_json)
        self.assertEqual(len(errors), 1)
        self.assertIn("tool-never-locked", errors[0])
        self.assertIn("no entry for it at all", errors[0])

    def test_registry_resolvable_spec_is_not_flagged_as_missing(self):
        packages_by_name = {"espressif32": {"url": "https://example.com/platform.zip"}}
        platform_json = {"packages": {
            "some-lib": {"version": "^1.2.3"},  # not a URI install
        }}
        self.assertEqual(vpp.cross_check_manifest_against_platform_json(packages_by_name, platform_json), [])

    def test_url_drift_on_a_tracked_package_is_an_error(self):
        packages_by_name = {
            "espressif32": {"url": "https://example.com/platform.zip"},
            "tool-foo": {"url": "https://example.com/foo-1.0.zip"},
        }
        platform_json = {"packages": {
            "tool-foo": {"version": "https://example.com/foo-2.0.zip"},  # bumped, not re-locked
        }}
        errors = vpp.cross_check_manifest_against_platform_json(packages_by_name, platform_json)
        self.assertEqual(len(errors), 1)
        self.assertIn("tool-foo", errors[0])

    def test_resolved_indirectly_package_resolved_url_mismatch_is_not_flagged(self):
        # tool-scons: the manifest pins the real resolved artifact, not the
        # wrapper URL platform.json declares -- comparing entry["url"]
        # directly against platform.json's wrapper URL would always mismatch
        # by design and must not be flagged. See wrapper_url handling below
        # for the check that actually matters for this package.
        packages_by_name = {
            "espressif32": {"url": "https://example.com/platform.zip"},
            "tool-scons": {
                "url": "https://example.com/scons-local-4.8.1.tar.gz",
                "wrapper_url": "https://example.com/scons-wrapper.zip",
            },
        }
        platform_json = {"packages": {
            "tool-scons": {"version": "https://example.com/scons-wrapper.zip"},
        }}
        self.assertEqual(vpp.cross_check_manifest_against_platform_json(packages_by_name, platform_json), [])

    def test_resolved_indirectly_package_wrapper_url_drift_is_flagged(self):
        # Regression test for the gap CodeRabbit's review caught: a wrapper
        # URL bump for tool-scons (which idf_tools.py-style resolution would
        # follow on the next real `pio run`) must be detected even though
        # the manifest pins the resolved artifact, not the wrapper.
        packages_by_name = {
            "espressif32": {"url": "https://example.com/platform.zip"},
            "tool-scons": {
                "url": "https://example.com/scons-local-4.8.1.tar.gz",
                "wrapper_url": "https://example.com/scons-wrapper-old.zip",
            },
        }
        platform_json = {"packages": {
            "tool-scons": {"version": "https://example.com/scons-wrapper-NEW.zip"},
        }}
        errors = vpp.cross_check_manifest_against_platform_json(packages_by_name, platform_json)
        self.assertEqual(len(errors), 1)
        self.assertIn("tool-scons", errors[0])
        self.assertIn("wrapper URL", errors[0])

    def test_resolved_indirectly_package_missing_wrapper_url_is_flagged(self):
        # A manifest entry generated before wrapper_url tracking existed
        # must be flagged as unable to detect drift, not silently trusted.
        packages_by_name = {
            "espressif32": {"url": "https://example.com/platform.zip"},
            "tool-scons": {"url": "https://example.com/scons-local-4.8.1.tar.gz"},
        }
        platform_json = {"packages": {
            "tool-scons": {"version": "https://example.com/scons-wrapper.zip"},
        }}
        errors = vpp.cross_check_manifest_against_platform_json(packages_by_name, platform_json)
        self.assertEqual(len(errors), 1)
        self.assertIn("no recorded wrapper_url", errors[0])

    def test_package_no_longer_declared_by_platform_json_is_not_an_error(self):
        # Manifest drift to clean up on next --update, not a security failure.
        packages_by_name = {
            "espressif32": {"url": "https://example.com/platform.zip"},
            "tool-removed": {"url": "https://example.com/removed-1.0.zip"},
        }
        platform_json = {"packages": {}}
        self.assertEqual(vpp.cross_check_manifest_against_platform_json(packages_by_name, platform_json), [])


if __name__ == "__main__":
    unittest.main()
