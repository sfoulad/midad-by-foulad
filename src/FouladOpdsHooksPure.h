#pragma once

#include <string>

// The subset of FouladOpdsHooks with zero OpdsParser/OpdsServerStore/HAL
// dependency, split into its own header so it's host-testable without any
// library include paths beyond <string> -- see test/foulad_opds_hooks_pure/.
// Implemented in FouladOpdsHooksPure.cpp; declarations re-exported from
// FouladOpdsHooks.h so callers only ever need to include that one header.
namespace FouladOpdsHooks {

// Foulad eBooks' catalog feed writes each book entry's <id> as
// "urn:opds-library:book:<numeric id>" (see foulad-ebooks'
// resources/views/opds/books.blade.php) -- the same numeric id used for
// device reading-stats reporting (EINK_DEVICE_TRACKING_TASKS.md). Returns ""
// for anything that doesn't end in a run of digits (a non-Foulad OPDS
// server's own id convention, a News entry's "urn:midad:feed:<id>", or a
// malformed entry) so callers can treat that as "no Foulad book id
// available" rather than a parse error.
std::string extractFouladBookId(const std::string& entryId);

// File extension to save an acquisition link's target under, based on
// foulad-ebooks' one-format-per-book MIME convention (XTC-only books are
// Arabic/PDF-sourced and recognized as downloadable BOOK entries too).
// Defaults to ".epub" for anything else.
std::string acquisitionExtension(const std::string& acquisitionType);

// True when `serverUrl` is the Foulad eBooks News feed root.
bool isNewsFeed(const std::string& serverUrl);

}  // namespace FouladOpdsHooks
