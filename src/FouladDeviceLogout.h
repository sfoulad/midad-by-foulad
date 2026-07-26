#pragma once

#include <string>

// Removing this device from the user's Foulad One account when they sign out.
//
// Signing out used to be purely local -- it dropped the stored credential and made
// no network call, so the device row and its token stayed alive server-side and the
// unit kept appearing under "My Devices". This is the other half.
//
// Deliberately server-confirmed: the caller must not clear the local credential
// until removeThisDevice() reports success. A device that believes it is signed out
// while the server still lists it is the same bug in reverse, and harder to notice.
namespace FouladDeviceLogout {

enum class Result : uint8_t {
  // Server confirmed the device is gone. Safe to clear the local credential.
  Removed,
  // The server has no row for this serial -- already removed from the phone app or
  // the web. Nothing to do, and the desired end state already holds, so this is
  // also safe to clear on.
  NotFound,
  // Offline, unauthenticated, or the server refused. The credential MUST be kept.
  Failed,
};

// Looks this device up by its own serial number and deletes it.
//
// Two round trips, because the delete addresses the device by its server-side id
// and the firmware only knows its serial: GET the account's device list, match on
// serial_number, then DELETE that id. Blocking; expect a couple of seconds.
//
// `username`/`password` are the stored Foulad eBooks OPDS credentials -- the app API
// sits behind the same Basic Auth (see FOULAD_EBOOKS_APP_DEVICES_URL).
Result removeThisDevice(const std::string& username, const std::string& password);

}  // namespace FouladDeviceLogout
