#include "FouladDeviceLogout.h"

#include <Logging.h>
#include <WiFi.h>

#include "FouladEbooksConfig.h"
#include "network/CoreCaCerts.h"
#include "network/HttpDownloader.h"

FouladDeviceLogout::Result FouladDeviceLogout::removeThisDevice(const std::string& username,
                                                                const std::string& password) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("LOGOUT", "no WiFi; refusing to sign out");
    return Result::Failed;
  }
  if (username.empty() || password.empty()) {
    LOG_ERR("LOGOUT", "no stored credential to authenticate with");
    return Result::Failed;
  }

  // wolfSSL against the pinned GTS Root R4 anchor, exactly as device-login
  // start/poll do -- NOT the esp_crt_bundle path.
  //
  // This endpoint is on the same host as pairing, whose chain is pure ECDSA
  // (leaf <- WE1 <- GTS Root R4). Going through the bundle made mbedTLS verify
  // against a bundled RSA root instead, and the RSA modular exponentiation could
  // not allocate: observed on the X3 as
  //   esp-x509-crt-bundle: PK verify failed with error 0x4290
  // which decodes as RSA_PUBLIC_FAILED + MPI_ALLOC_FAILED, with free heap driven
  // to 128 bytes mid-handshake. It reads like a trust failure and is not one --
  // "Certificate matched" means the anchor WAS found. Rebooting into a clean heap
  // first did not help, because the cost is inside the verification itself.
  std::string response;
  if (HttpDownloader::postJsonWolf(FOULAD_EBOOKS_DEVICE_SIGNOUT_URL, "{}", core_ca::kMidadCoreCaGtsRootR4, response,
                                   15000, username, password) == HttpDownloader::OK) {
    LOG_INF("LOGOUT", "this device's token revoked via /device-login/self-revoke");
    return Result::Removed;
  }

  const auto failure = HttpDownloader::getLastFailure();
  if (failure.stage != HttpDownloader::FailStage::STATUS) {
    // Transport failure: nothing was decided server-side, so the token may well
    // still be live. Keeping it is the only safe reading.
    LOG_ERR("LOGOUT", "signout transport failure (stage=%d)", static_cast<int>(failure.stage));
    return Result::Failed;
  }

  switch (failure.detail) {
    case 401:
      // The credential that authenticated this call is not live: already revoked from
      // the phone, or the account is gone. The end state the user asked for already
      // holds server-side, and keeping a dead token locally only leaves the reader
      // unable to sign out at all -- so this is success, not a refusal.
      LOG_INF("LOGOUT", "credential already revoked server-side; clearing it locally");
      return Result::NotFound;
    case 429:
      // Rate limited per IP. Nothing was revoked, so the token must be kept.
      LOG_ERR("LOGOUT", "signout rate limited; retry in a minute");
      return Result::Failed;
    default:
      LOG_ERR("LOGOUT", "signout refused with status %d", failure.detail);
      return Result::Failed;
  }
}
