"""Unit tests for scripts/check-ota-signing-flags.py, using synthetic
platformio.ini fixtures rather than the real one -- so a real, correctly
flagged platformio.ini can't hide a regression in the check logic itself."""

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parent.parent / "check-ota-signing-flags.py"
spec = importlib.util.spec_from_file_location("check_ota_signing_flags", MODULE_PATH)
check_ota_signing_flags = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check_ota_signing_flags)


def write_ini(tmp_dir, content):
    path = Path(tmp_dir) / "platformio.ini"
    path.write_text(content)
    return path


class CheckOtaSigningFlagsTest(unittest.TestCase):
    def test_all_release_envs_flagged_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = write_ini(tmp, """
[env:default]
build_flags = -DSOMETHING=1

[env:gh_release]
build_flags = -DOTA_SIGNING_BOOT_CHECK_ENABLED=1 -DLOG_LEVEL=0

[env:sticky-gh_release_rc]
build_flags = -DOTA_SIGNING_BOOT_CHECK_ENABLED=1 -DLOG_LEVEL=1
""")
            code, message = check_ota_signing_flags.check(ini)
            self.assertEqual(code, 0)
            self.assertIn("OK", message)

    def test_missing_flag_on_one_release_env_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = write_ini(tmp, """
[env:default]
build_flags = -DSOMETHING=1

[env:gh_release]
build_flags = -DOTA_SIGNING_BOOT_CHECK_ENABLED=1

[env:x4pro-gh_release]
build_flags = -DLOG_LEVEL=0
""")
            code, message = check_ota_signing_flags.check(ini)
            self.assertEqual(code, 1)
            self.assertIn("[env:x4pro-gh_release]", message)
            # The correctly flagged env must not be reported as missing.
            self.assertNotIn("[env:gh_release]", message)

    def test_missing_build_flags_key_entirely_fails(self):
        # A release section with no build_flags key at all (not just an empty
        # one) must fail the same way as a section missing the flag.
        with tempfile.TemporaryDirectory() as tmp:
            ini = write_ini(tmp, """
[env:gh_release_rc]
platform = native
""")
            code, message = check_ota_signing_flags.check(ini)
            self.assertEqual(code, 1)
            self.assertIn("gh_release_rc", message)

    def test_no_release_sections_at_all_fails_closed(self):
        # Guards the guard itself: if the section-matching pattern breaks
        # (e.g. a future rename), this must fail loudly, not silently pass
        # with zero sections checked.
        with tempfile.TemporaryDirectory() as tmp:
            ini = write_ini(tmp, """
[env:default]
build_flags = -DSOMETHING=1
""")
            code, message = check_ota_signing_flags.check(ini)
            self.assertEqual(code, 1)
            self.assertIn("no *gh_release", message)

    def test_non_release_env_missing_flag_is_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = write_ini(tmp, """
[env:default]
build_flags = -DSOMETHING=1

[env:gh_release]
build_flags = -DOTA_SIGNING_BOOT_CHECK_ENABLED=1
""")
            code, _ = check_ota_signing_flags.check(ini)
            self.assertEqual(code, 0)


class IsReleaseEnvTest(unittest.TestCase):
    def test_matches_expected_suffixes(self):
        self.assertTrue(check_ota_signing_flags.is_release_env("env:gh_release"))
        self.assertTrue(check_ota_signing_flags.is_release_env("env:sticky-gh_release_rc"))
        self.assertTrue(check_ota_signing_flags.is_release_env("env:papermono-gh_release"))

    def test_rejects_non_release_and_non_env_sections(self):
        self.assertFalse(check_ota_signing_flags.is_release_env("env:default"))
        self.assertFalse(check_ota_signing_flags.is_release_env("env:simulator"))
        self.assertFalse(check_ota_signing_flags.is_release_env("base"))
        # Must not match on substring alone -- a section that merely contains
        # "gh_release" without the exact suffix should not pass.
        self.assertFalse(check_ota_signing_flags.is_release_env("env:gh_release_experimental"))


if __name__ == "__main__":
    unittest.main()
