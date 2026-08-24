"""Tests for scripts/sign_firmware.sh: the fail-closed guard clauses always
run (no external tooling needed), and the full sign/verify/tamper/wrong-key/
unsigned matrix runs whenever espsecure.py is on PATH (it is in CI, installed
from scripts/requirements-ci.lock; skipped with a clear reason otherwise).

This exercises the real script via subprocess, not a reimplementation of its
logic -- a regression in the actual shipped script is what this needs to catch.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "scripts" / "sign_firmware.sh"

HAS_ESPSECURE = shutil.which("espsecure.py") is not None


def run_script(args, env_overrides=None, cwd=None):
    env = os.environ.copy()
    if env_overrides is not None:
        env = env_overrides
    return subprocess.run(
        ["bash", str(SCRIPT), *args],
        cwd=cwd,
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )


class SignFirmwareFailClosedTest(unittest.TestCase):
    """These guard clauses (scripts/sign_firmware.sh:16-39) must always run,
    independent of whether espsecure.py is installed."""

    def test_wrong_arg_count_fails(self):
        result = run_script(["only-one-arg"], env_overrides={})
        self.assertEqual(result.returncode, 1)
        self.assertIn("Usage:", result.stderr)

    def test_missing_signing_key_env_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            fw = Path(tmp) / "firmware.bin"
            pub = Path(tmp) / "public-key.pem"
            fw.write_bytes(b"dummy")
            pub.write_bytes(b"dummy")
            env = {k: v for k, v in os.environ.items() if k != "OTA_SIGNING_KEY"}
            result = run_script([str(fw), str(pub)], env_overrides=env)
            self.assertEqual(result.returncode, 1)
            self.assertIn("OTA_SIGNING_KEY is not set", result.stderr)
            self.assertIn("refusing to publish an unsigned release", result.stderr)

    def test_missing_public_key_file_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            fw = Path(tmp) / "firmware.bin"
            fw.write_bytes(b"dummy")
            missing_pub = Path(tmp) / "does-not-exist.pem"
            env = dict(os.environ, OTA_SIGNING_KEY="not-a-real-key")
            result = run_script([str(fw), str(missing_pub)], env_overrides=env)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Public verification key", result.stderr)
            self.assertIn("not found", result.stderr)

    def test_missing_firmware_file_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            pub = Path(tmp) / "public-key.pem"
            pub.write_bytes(b"dummy")
            missing_fw = Path(tmp) / "does-not-exist.bin"
            env = dict(os.environ, OTA_SIGNING_KEY="not-a-real-key")
            result = run_script([str(missing_fw), str(pub)], env_overrides=env)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Firmware artifact", result.stderr)
            self.assertIn("not found", result.stderr)


@unittest.skipUnless(HAS_ESPSECURE, "espsecure.py not on PATH -- install esptool "
                                     "(scripts/requirements-ci.lock) to run this suite")
class SignFirmwareRoundTripTest(unittest.TestCase):
    """The actual security property this whole mechanism exists for: a real
    key signs and verifies; tampering, wrong keys, and unsigned images are all
    rejected. Verified manually against espsecure.py 4.12.0 (the exact pinned
    version) before being encoded here -- see issue #179 Round 1 groundwork."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

        self.real_key = self.dir / "real-key.pem"
        self.wrong_key = self.dir / "wrong-key.pem"
        self.real_pub = self.dir / "real-pub.pem"
        self.wrong_pub = self.dir / "wrong-pub.pem"
        for keyfile in (self.real_key, self.wrong_key):
            subprocess.run(
                ["espsecure.py", "generate_signing_key", "--version", "2",
                 "--scheme", "rsa3072", str(keyfile)],
                check=True, capture_output=True, text=True,
            )
        for keyfile, pubfile in ((self.real_key, self.real_pub), (self.wrong_key, self.wrong_pub)):
            subprocess.run(
                ["espsecure.py", "extract_public_key", "--version", "2",
                 "--keyfile", str(keyfile), str(pubfile)],
                check=True, capture_output=True, text=True,
            )

        self.firmware = self.dir / "firmware.bin"
        self.firmware.write_bytes(os.urandom(65536))

    def _verify(self, pubkey, datafile):
        return subprocess.run(
            ["espsecure.py", "verify_signature", "--version", "2",
             "--keyfile", str(pubkey), str(datafile)],
            capture_output=True, text=True,
        )

    def test_valid_signature_is_accepted(self):
        env = dict(os.environ, OTA_SIGNING_KEY=self.real_key.read_text())
        result = run_script([str(self.firmware), str(self.real_pub)], env_overrides=env)
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("Signed and verified", result.stdout)
        # sign_firmware.sh already self-verifies (script line 60); confirm
        # independently too, since that's the actual security property.
        verify = self._verify(self.real_pub, self.firmware)
        self.assertEqual(verify.returncode, 0, msg=verify.stderr)

    def test_wrong_key_is_rejected(self):
        env = dict(os.environ, OTA_SIGNING_KEY=self.real_key.read_text())
        result = run_script([str(self.firmware), str(self.real_pub)], env_overrides=env)
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        verify = self._verify(self.wrong_pub, self.firmware)
        self.assertNotEqual(verify.returncode, 0)

    def test_unsigned_firmware_is_rejected(self):
        # Never signed at all -- verify_signature must reject it, not
        # silently treat "no signature" as "trivially valid."
        verify = self._verify(self.real_pub, self.firmware)
        self.assertNotEqual(verify.returncode, 0)

    def test_tampered_firmware_is_rejected(self):
        env = dict(os.environ, OTA_SIGNING_KEY=self.real_key.read_text())
        result = run_script([str(self.firmware), str(self.real_pub)], env_overrides=env)
        self.assertEqual(result.returncode, 0, msg=result.stderr)

        tampered = self.dir / "tampered.bin"
        data = bytearray(self.firmware.read_bytes())
        data[100] ^= 0xFF  # flip a single bit past the signature block
        tampered.write_bytes(data)

        verify = self._verify(self.real_pub, tampered)
        self.assertNotEqual(verify.returncode, 0)
        self.assertIn("does not match", verify.stdout + verify.stderr)

    def test_signing_fails_closed_if_key_file_write_fails(self):
        # OTA_SIGNING_KEY set but garbage (not a real PEM) -- espsecure.py
        # must reject it, and the script must propagate that failure (no
        # `|| true`-style swallowing anywhere in sign_firmware.sh).
        env = dict(os.environ, OTA_SIGNING_KEY="not a real PEM key at all")
        result = run_script([str(self.firmware), str(self.real_pub)], env_overrides=env)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
