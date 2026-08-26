# OTA Signing Key Status: PRE-PRODUCTION

**Status:** pre-production / release-candidate key -- NOT the stable
production signing key.

**Authorized uses:** CI, signed release-candidate (RC) generation, and
hardware qualification testing only.

**NOT authorized for:** signing a stable production release shipped to
customer devices. Provisioning this key is not, by itself, a decision to
ship a stable release signed with it.

**Custody model:** a single locally-encrypted backup stored in a
user-selected iCloud Drive folder -- not the dual-independent-physical-drive
custody model used for the production key (see
docs/ota-production-key-ceremony.md). This reduced custody was accepted
because no production customer devices depend on this key yet; affected
test/RC hardware can be physically re-flashed if the key is lost.

**Before public production launch:** the project will decide whether to
retain this key with strengthened custody (the full dual-USB production
ceremony) or rotate to a newly-generated production key with proper custody
from the start. See docs/ota-signing-key-management.md's key rotation
procedure.

**Public key fingerprint (SHA-256 of DER-encoded key):** 7d27a7d01082e7d42ac74de1bdcee04d4406794e86df54a0d013572859cd4290

**Provisioned:** 2026-08-26 via `scripts/ota-key-ceremony.sh --preproduction`
