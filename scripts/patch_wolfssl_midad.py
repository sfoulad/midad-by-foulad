"""
PlatformIO pre-build script: Midad-specific wolfSSL config additions.

Runs after scripts/patch_wolfssl.py (CrossPoint-owned; keep that file
byte-identical to upstream and put anything Midad-specific here instead --
see docs/upstream-sync-architecture.md's Midad Thin-Fork Architecture
section). Uses the same idempotent marker-based append pattern as that
script, just with a distinct marker so re-running either script never
duplicates or clobbers the other's block.
"""

from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* Midad wolfSSL TLS-chain overrides */"
OVERRIDES = f"""

{MARKER}
/* Some public CAs serve a redundant self-referential CA cert as the last
   link in their chain (e.g. SSL.com sends a copy of "SSL.com TLS ECC Root
   CA 2022" cross-signed by the legacy "AAA Certificate Services" root,
   even to clients that already trust the 2022 root directly). wolfSSL's
   default strict chain walk (internal.c's ProcessPeerCerts) tries to
   verify that trailing cert's own signature and fails with
   ASN_NO_SIGNER_E when its issuer isn't also in the trust store -- even
   though the leaf already validates to a directly-trusted anchor one hop
   earlier. WOLFSSL_ALT_CERT_CHAINS (internal.c's own comment: "its okay
   for a CA cert to fail with ASN_NO_SIGNER_E here... only requires that
   the peer certificate validate to a trusted CA") tolerates exactly this
   case without weakening leaf verification. Confirmed via hardware
   (default_tls_measure on an X3): a 12-root production-candidate bundle
   (src/debug/TlsHeapMeasureCaBundles.h) already contains the correct
   self-signed root and fails 10/10 on this exact error before this
   define, independently of bundle content. */
#ifndef WOLFSSL_ALT_CERT_CHAINS
#define WOLFSSL_ALT_CERT_CHAINS
#endif
/* platformio.ini passes -DWOLFSSL_OPTIONS_H alongside -DWOLFSSL_USER_SETTINGS.
   That looks like it should pull in wolfSSL's own wolfssl/options.h (which
   sets WOLFSSL_SHA384/WOLFSSL_SHA512/WOLFSSL_SHA224 among other defaults),
   but options.h's own include guard is `#ifndef WOLFSSL_OPTIONS_H` -- defining
   that name as a build flag satisfies the guard before the file is ever
   opened, so its entire body (including those defines) is silently skipped.
   settings.h only ever includes options.h itself under EXTERNAL_OPTS_OPENVPN,
   which this project doesn't set, so user_settings.h (included unconditionally
   via WOLFSSL_USER_SETTINGS) is the only config that actually takes effect.
   That file's own comment ("optionally turn off SHA512/224 SHA512/256")
   assumed SHA384/512 were already on -- they were not. Confirmed via
   hardware: without this define, verifying any certificate signed with
   ecdsa-with-SHA384 or sha384WithRSAEncryption (SSL.com's ECC intermediates
   among them -- openssl x509 -text on example.com's live chain shows
   exactly that algorithm two links up) fails wolfSSL's HashForSignature()
   with HASH_TYPE_E (-232), independent of which roots are in the trust
   store. Re-adding just the specific defines the original design relied
   on, rather than fixing the dead -DWOLFSSL_OPTIONS_H flag itself (which
   would silently re-enable every other option.h default too, unaudited)
   keeps this change narrowly scoped. */
#ifndef WOLFSSL_SHA384
#define WOLFSSL_SHA384
#endif
#ifndef WOLFSSL_SHA512
#define WOLFSSL_SHA512
#endif
/* sp_c32.c's sp_ecc_verify_384() (the fast SP path already used for P-256
   verification) is entirely gated behind `#ifdef WOLFSSL_SP_384`, which this
   build never defined despite WOLFSSL_HAVE_SP_ECC + WOLFSSL_SP_SMALL being
   set -- so P-384 fell back to wolfSSL's generic (non-SP) bignum ECC verify
   path. Confirmed via hardware (default_tls_measure on an X3): without this
   define, verifying an ecdsa-with-SHA384 signature (SSL.com's ECC
   intermediate chain, common among public CAs) deterministically fails
   ConfirmSignature() with ASN_SIG_CONFIRM_E (-155) on 10/10 attempts --
   the generic path's P-384 verification is broken in this build's specific
   configuration, not the certificates or the earlier fixes above. Defining
   WOLFSSL_SP_384 routes P-384 through the same fast/correct SP path P-256
   already uses and fixed 10/10 handshakes for ~10KB flash. */
#ifndef WOLFSSL_SP_384
#define WOLFSSL_SP_384
#endif
"""


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings (Midad): {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)
