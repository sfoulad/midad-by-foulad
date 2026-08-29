#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

// Pure decision logic shared by the two firmware-update offer paths: the
// Settings "Check for updates" flow (OtaUpdater) and the Library offer parsed
// out of the catalog server's registerDevice() response (FouladDeviceTracking).
// Header-only and free of Arduino/IDF dependencies so the host test suite
// (test/firmware_update_policy) exercises the identical code the firmware runs
// -- a second implementation of "is this newer" is how the two paths came to
// disagree.
namespace firmware_update_policy {

// Asset name every release carries: the C3 X4/X3 image.
inline constexpr char DEFAULT_FIRMWARE_ASSET[] = "firmware.bin";

// Release asset filename for a board (FirmwareBoardTag name + length): plain
// firmware.bin for the C3 "x4" image, firmware-<board>.bin for every other
// board. Single source for both offer paths and the release workflows' naming.
inline void boardAssetFileName(const char* boardName, const size_t boardNameLen, char* out, const size_t outCap) {
  if (boardNameLen == 2 && memcmp(boardName, "x4", 2) == 0) {
    snprintf(out, outCap, "%s", DEFAULT_FIRMWARE_ASSET);
  } else {
    snprintf(out, outCap, "firmware-%.*s.bin", static_cast<int>(boardNameLen), boardName);
  }
}

// True when `candidate` (a release tag, leading 'v'/'V' optional) names a build
// newer than `current` (CROSSPOINT_VERSION format: "1.8.57" or
// "1.8.57-rc+254abcd").
//
// Ordering is by the numeric major.minor.patch triple. On a tie, the one
// legitimate upgrade is the stable release that finalizes the -rc this device
// is running. Another -rc tag with the same triple is the SAME version: build
// metadata after '+' never orders two builds (it is a commit SHA, and the same
// commit has appeared with both 7- and 8-character truncations), so treating
// any same-numbered tag as newer re-offered a device its own running build
// forever.
inline bool isVersionNewer(const char* candidate, const char* current) {
  if (candidate == nullptr || *candidate == '\0') return false;
  if (current == nullptr || *current == '\0') return false;

  // GitHub tags are conventionally "v1.6.24" while CROSSPOINT_VERSION is the
  // bare "1.6.24" -- normalize both sides so the prefix alone never makes two
  // spellings of one version look different.
  if (*candidate == 'v' || *candidate == 'V') candidate++;
  if (*current == 'v' || *current == 'V') current++;

  if (strcmp(candidate, current) == 0) return false;

  int candMajor = 0, candMinor = 0, candPatch = 0;
  int curMajor = 0, curMinor = 0, curPatch = 0;
  // sscanf return values checked: a tag that doesn't parse as X.Y.Z can never
  // justify flashing, and unparsed output variables would otherwise be read as
  // stack garbage (handing sscanf a string starting with a non-digit parses
  // zero fields).
  if (sscanf(candidate, "%d.%d.%d", &candMajor, &candMinor, &candPatch) != 3) return false;
  if (sscanf(current, "%d.%d.%d", &curMajor, &curMinor, &curPatch) != 3) return false;

  if (candMajor != curMajor) return candMajor > curMajor;
  if (candMinor != curMinor) return candMinor > curMinor;
  if (candPatch != curPatch) return candPatch > curPatch;

  // Equal triple: only "running the -rc, offered the stable of the same
  // number" is an upgrade.
  const bool currentIsRc = strstr(current, "-rc") != nullptr;
  const bool candidateIsRc = strstr(candidate, "-rc") != nullptr;
  return currentIsRc && !candidateIsRc;
}

// One release channel as reported by the catalog server: its tag, plus the
// release's asset filenames when the server lists them (`assetsListed`). No
// list means "no information", not "no assets".
struct ChannelInfo {
  const char* tag = nullptr;
  const char* const* assets = nullptr;
  size_t assetCount = 0;
  bool assetsListed = false;
};

enum class OfferOutcome : unsigned char {
  NONE,                // nothing newer on any eligible channel
  OFFER,               // `tag` should be offered
  MISSING_BOARD_ASSET  // a newer release exists, but none carries this board's asset
};

struct OfferDecision {
  OfferOutcome outcome = OfferOutcome::NONE;
  const char* tag = nullptr;
};

// Whether a channel's release can actually update this board. With an asset
// list, the board's own asset must be in it. Without one, only the default
// firmware.bin boards qualify: every release carries firmware.bin, while
// per-board assets (firmware-x4pro.bin, ...) exist only in releases built for
// them -- offering an unverifiable release to a non-default board is how the
// X4 Pro got a dead-end prompt for a C3-only release.
inline bool channelHasBoardAsset(const ChannelInfo& channel, const char* boardAsset) {
  if (!channel.assetsListed) {
    return strcmp(boardAsset, DEFAULT_FIRMWARE_ASSET) == 0;
  }
  for (size_t i = 0; i < channel.assetCount; i++) {
    if (channel.assets[i] != nullptr && strcmp(channel.assets[i], boardAsset) == 0) return true;
  }
  return false;
}

// Picks the build to offer, or reports why none is offerable. Channel order
// matches what "Check for updates" has always done: the rc channel is
// consulted only when the device wants pre-releases, and the stable tag is
// always the fallback (it is what carries an RC device off a pre-release once
// the equal-numbered release lands). A channel that is newer but lacks this
// board's asset is skipped -- reported as MISSING_BOARD_ASSET when nothing
// else is offerable, so the suppression is distinguishable from "nothing
// newer" in the debug log.
inline OfferDecision chooseOffer(const ChannelInfo& rc, const ChannelInfo& stable, const bool wantsPrerelease,
                                 const char* current, const char* boardAsset) {
  OfferDecision decision;
  const ChannelInfo* channels[2] = {wantsPrerelease ? &rc : nullptr, &stable};
  for (const ChannelInfo* channel : channels) {
    if (channel == nullptr || channel->tag == nullptr) continue;
    if (!isVersionNewer(channel->tag, current)) continue;
    if (!channelHasBoardAsset(*channel, boardAsset)) {
      decision.outcome = OfferOutcome::MISSING_BOARD_ASSET;
      continue;  // stable may still be offerable
    }
    decision.outcome = OfferOutcome::OFFER;
    decision.tag = channel->tag;
    return decision;
  }
  return decision;
}

}  // namespace firmware_update_policy
