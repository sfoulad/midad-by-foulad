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

// Revokes this device's own token.
//
// One round trip, and the request names no target: Core revokes whichever token
// authenticated the call, so there is no id or serial to send and no way to reach a
// sibling reader's credential. Blocking; expect a second or two.
//
// `username`/`password` are the stored credentials, sent as Basic auth exactly as
// the feed and download requests send them.
Result removeThisDevice(const std::string& username, const std::string& password);

}  // namespace FouladDeviceLogout
