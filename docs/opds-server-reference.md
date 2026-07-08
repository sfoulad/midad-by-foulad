# Foulad eBooks OPDS server reference

Authoritative reference for the server-side OPDS implementation this app's OPDS
client (`lib/OpdsParser/`, `src/network/HttpDownloader.cpp`,
`src/activities/browser/OpdsBookBrowserActivity.cpp`, `src/OpdsCoverCache.cpp`)
talks to. Pulled directly from the foulad-ebooks codebase, accurate as of
foulad-ebooks v0.11.2. Keep this in sync if the server contract changes --
it's meant to save re-deriving server behavior via curl each time the client
needs a fix.

## Authentication

Two methods, both handled by `OpdsBasicAuth` middleware on every OPDS route:

1. **HTTP Basic Auth** -- `Authorization: Basic base64(username:password)`,
   checked against the app's own `users` table (same credentials as the admin
   web login, not separate). Returns `401` with
   `WWW-Authenticate: Basic realm="Foulad eBooks"` on failure.
2. **Laravel signed URLs** -- used for cover/download links embedded in feeds
   (many OPDS clients don't resend `Authorization` on every request). These
   carry a `signature` query param plus an embedded `user` param identifying
   who the link was signed for. No `Authorization` header needed for these.

## Endpoints

| Route | Purpose |
|---|---|
| `GET /opds/` | Root navigation feed (categories list) |
| `GET /opds/books` | Acquisition feed: all books, paginated |
| `GET /opds/category/{slug}` | Navigation feed: one category's subcategories, "All Books", "Recently Added" |
| `GET /opds/category/{slug}/all` | Acquisition feed: every book in that category's subtree, paginated |
| `GET /opds/category/{slug}/recent` | Acquisition feed: same subtree, newest upload first, paginated |
| `GET /opds/recent` | Acquisition feed: every visible book, newest upload first, paginated |
| `GET /opds/search?q=...` | Acquisition feed: title/author match, paginated |
| `GET /opds/books/{id}/download` | Download the EPUB/PDF source file |
| `GET /opds/books/{id}/xtc` | Download the converted `.xtc`/`.xtch` file |
| `GET /opds/books/{id}/cover` | Cover image (black & white PNG) |

## Navigation feeds (root and every `/opds/category/{slug}`)

**As of server v0.10.14, every category feed is pure navigation, the same way
the root feed itself works** -- neither ever lists a real book `<entry>`
directly. Standard Atom/OPDS navigation feed: `<link rel="subsection">`
entries only, no acquisition-format book entries mixed in.

- **Root** (`/opds/`): "All Books", "Recently Added", then one entry per
  non-empty top-level category.
- **A category** (`/opds/category/{slug}`): any non-empty subcategories,
  then "All Books" and "Recently Added" entries scoped to that category's own
  subtree (itself + every descendant subcategory combined) -- these are the
  *only* way to reach real book entries for that category. This applies
  uniformly whether the category is a top-level language (English/Arabic) or
  a leaf subcategory with no children of its own -- a leaf category's page no
  longer lists its books directly, it shows these two entries instead.
- Categories/subcategories with zero visible books are omitted entirely (no
  dead-end links).

## Acquisition feeds (`/opds/books`, `/opds/category/{slug}/all`, `/opds/category/{slug}/recent`, `/opds/recent`, `/opds/search`)

- `<link rel="self">` and `rel="start"` always present.
- **Pagination**: 25 books per page. `<link rel="next">`/`rel="previous"`
  appear as standard `<link>` elements at the feed level (not per-entry) when
  more pages exist -- just a plain `?page=N` query param appended to the same
  URL. No pagination link at all if everything fits on one page.
- No `<link rel="subsection">` entries ever appear in these feeds -- that's
  exclusively a navigation-feed concept now (see above). These feeds are
  book entries only.
- Each book is one `<entry>`: `<title>`, `<id>`
  (`urn:opds-library:book:{id}`), `<updated>`, optional `<author><name>`,
  optional `<summary>` (HTML already stripped), one
  `<category term="{slug}" label="{name}">` per tag.

## Cover images

- `<link rel="http://opds-spec.org/image">` and
  `rel="http://opds-spec.org/image/thumbnail">` -- **currently both point to
  the identical URL/size** (no separate full-size vs thumbnail variant).
- URL: a Laravel signed route, `GET /opds/books/{id}/cover?signature=...&user=...`
  (embedded directly in the feed entry -- the device never needs to construct
  this URL itself, just fetch whatever href the feed gives it).
- Format: `image/png`, black & white (2-color palette PNG, values pure
  `0,0,0` / `255,255,255` -- visually 1-bit, not a grayscale ramp), **as of
  v0.10.11: 160x240 max** (was 240x360 before that, was 480x800 before that
  -- if a cached cover looks stale/oversized on-device, that's almost always
  a client-side cache issue, not the server serving an old size -- see
  "Server-side cover caching" below).
- **Server never upscales**: the resize is `min(160/w, 240/h, 1.0)` -- a
  source cover already smaller than 160x240 is served at its original
  (smaller) size, not padded or stretched up to fill the full 160x240. If the
  device's cover-rendering path assumes every OPDS cover is exactly 160x240
  and mishandles a smaller image (e.g. leaves stale pixels around a
  smaller-than-expected bitmap instead of clearing the cell first), that
  would explain a cover appearing to "not show" for specific books while
  others work fine. Worth checking against a real cover response's actual
  `IHDR` dimensions (`file <(curl ...)` or a hex dump of the PNG header) if
  this is suspected, rather than assuming the fixed grid-cell size.
- Grayscale conversion is a plain 50% threshold, not dithered (dropped
  deliberately in v0.10.3 -- dithering's pixel noise defeated PNG
  compression for negligible visible gain at this size).
- Only present if the book actually has a cover; no `<link>` at all otherwise
  -- as of foulad-ebooks v0.10.13, all books on production were audited and
  confirmed to have one, but any book uploaded/converted since then could
  still be missing one if its source EPUB/PDF cover extraction failed
  silently (extraction bugs have happened before, see foulad-ebooks
  CHANGELOG v0.10.10).
- Cache header: `Cache-Control: private, max-age=604800` (7 days) -- but the
  signed URL itself is valid 30 days, so caching the image response
  separately from re-fetching the feed is safe.

### Server-side cover caching (why a size bump might not show up immediately)

The server generates the black & white PNG once per book and caches it on
disk at `covers-bw/v3/{book_id}.png` -- it is **not** regenerated on every
request. That `v3` segment is a manual version bump the foulad-ebooks side
increments whenever `CoverConverter`'s output changes (size, algorithm) to
force old cached files to regenerate; it has no bearing on anything the
device does, it's purely an internal cache-key. If covers still look wrong
after a foulad-ebooks deploy that claims to have changed cover generation,
that's a server-side question (did the cache path actually bump?), not
something fixable from the firmware side.

### If covers aren't showing on-device at all (not just wrong size)

Things confirmed **not** broken server-side as of v0.11.2 (checked directly
against the `OpdsController`/`CoverConverter`/Blade view source, not just
assumed): the signed URL is embedded correctly in every feed entry that has
a cover, the route returns a real `image/png` body with a 200 status when
fetched, and the black & white conversion produces a valid PNG. If covers
still don't render on a real device, the likely remaining suspects, roughly
in the order worth checking:
1. **Local device cover cache gone stale**: `.crosspoint/opds_covers/` (or
   wherever `OpdsCoverCache.cpp` persists fetched covers) may hold entries
   cached from *before* an earlier signed-URL/HTTPS bug was fixed
   server-side (see the HTTPS section below and foulad-ebooks' `v0.10.8`
   history) -- a failed fetch cached as a failure would never retry. Clear
   that cache and re-fetch fresh.
2. **The small-source-cover no-upscale behavior** described above, if it's
   specific books that fail rather than all of them.
3. **A genuinely missing `cover_path`** for a book uploaded after the
   v0.10.13 audit (server-side question, not a device bug) -- confirm by
   checking whether that book's feed entry has a `rel="...image"` link at
   all, since the server omits the link entirely (not a broken link) when
   there's no cover on file.

## Acquisition (download) link -- exactly one per book, never a choice

- `<link rel="http://opds-spec.org/acquisition">` with a `type` attribute the
  device should use to distinguish format:
  - **EPUB** (any language, including Arabic as of v0.10.4):
    `type="application/epub+zip"` -> hits `/opds/books/{id}/download`
  - **PDF, once converted**: `type="application/x-xtc"` or
    `type="application/x-xtch"` (exact extension of the stored file) -> hits
    `/opds/books/{id}/xtc`. **Both must be recognized as acquisition
    formats** -- confirmed already handled in `OpdsParser.cpp`
    (`isXtcAcquisition` checks both strings).
  - **PDF, not yet converted**: no acquisition link at all (book still shows
    title/cover, just nothing downloadable yet) -- don't treat a missing link
    as an error, some readers hide such entries entirely. Our client
    currently does hide them (`OpdsParser::endElement` only pushes an entry
    into `entries` when both title and href are non-empty) -- matches one of
    the explicitly-acceptable behaviors called out here, not a bug.
- A book with a PDF source never offers its raw PDF directly -- it's
  XTC-or-nothing.

## Signed URLs (covers + downloads)

- 30-day expiry (was 24 hours before v0.9.5 -- extended because readers cache
  feeds/links longer than that).
- Authorization is checked **live on every request** against current
  visibility rules, not baked into the signature at generation time -- so a
  link can still 403 even before it expires if the underlying book becomes
  inaccessible.

## HTTPS

Since v0.9.7, the server forced `https://` for all generated links and
redirected any plain `http://` request. **Temporarily suspended as of
2026-07 (beta only):** Let's Encrypt rotated foulad.one's certificate onto a
hierarchy ("ISRG Root YE") that ESP-IDF's embedded trust bundle doesn't
recognize, and firmware-side verification of it turned out to drive free
heap dangerously low and freeze the device (see foulad-eink's
`HttpDownloader.cpp` history) -- not safe to keep. The server has been
reconfigured to serve OPDS over plain `http://` without redirecting, so the
device can keep working during beta; `FouladEbooksConfig.h`'s
`FOULAD_EBOOKS_URL` was updated to `http://foulad.one/opds` to match. Basic
Auth credentials travel in cleartext over this connection as a result -- an
accepted tradeoff for beta, not something to carry into production. Revert
both sides (server back to forcing HTTPS, `FOULAD_EBOOKS_URL` back to
`https://`) once the certificate chain is fixed server-side or ESP-IDF
updates its bundle to include the new root.

This only matters for the hardcoded Foulad eBooks entry; a user's own
manually-entered OPDS server URL was never affected, since
`UrlUtils::ensureProtocol()` already defaults to `http://` for local-server
support.
