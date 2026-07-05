# Foulad eBooks OPDS server reference

Authoritative reference for the server-side OPDS implementation this app's OPDS
client (`lib/OpdsParser/`, `src/network/HttpDownloader.cpp`,
`src/activities/browser/OpdsBookBrowserActivity.cpp`, `src/OpdsCoverCache.cpp`)
talks to. Pulled directly from the foulad-ebooks codebase, accurate as of
foulad-ebooks v0.10.4. Keep this in sync if the server contract changes --
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
| `GET /opds/category/{slug}` | Acquisition feed: one category, paginated |
| `GET /opds/books/{id}/download` | Download the EPUB/PDF source file |
| `GET /opds/books/{id}/xtc` | Download the converted `.xtc`/`.xtch` file |
| `GET /opds/books/{id}/cover` | Cover image (black & white PNG) |

## Root feed (navigation)

Standard Atom/OPDS navigation feed. `<link rel="subsection">` for "All Books"
plus one entry per non-empty top-level category. Categories with zero visible
books are omitted entirely (no dead-end links).

## Acquisition feed (`/opds/books` or `/opds/category/{slug}`)

- `<link rel="self">` and `rel="start"` always present.
- **Pagination**: 25 books per page. `<link rel="next">`/`rel="previous"`
  appear as standard `<link>` elements at the feed level (not per-entry) when
  more pages exist -- just a plain `?page=N` query param appended to the same
  URL. No pagination link at all if everything fits on one page.
- If browsing a category with subcategories, those subcategories appear as
  `<link rel="subsection">` entries -- **only on page 1** of that category's
  feed, never repeated on later pages.
- Each book is one `<entry>`: `<title>`, `<id>`
  (`urn:opds-library:book:{id}`), `<updated>`, optional `<author><name>`,
  optional `<summary>` (HTML already stripped), one
  `<category term="{slug}" label="{name}">` per tag.

## Cover images

- `<link rel="http://opds-spec.org/image">` and
  `rel="http://opds-spec.org/image/thumbnail">` -- **currently both point to
  the identical URL/size** (no separate full-size vs thumbnail variant).
- Format: `image/png`, black & white (1-bit visually, though technically an
  8-bit palette PNG), **240x360 max**, plain threshold (no dithering).
- Only present if the book actually has a cover; no `<link>` at all otherwise.
- Cache header: `Cache-Control: private, max-age=604800` (7 days) -- but the
  signed URL itself is valid 30 days, so caching the image response
  separately from re-fetching the feed is safe.

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

Since v0.9.7, the server forces `https://` for all generated links and
redirects any plain `http://` request. If the device's HTTP client doesn't
handle a 30x redirect cleanly on a request with `Authorization` headers,
point its configured server URL directly at `https://foulad.one` to avoid
ever hitting that redirect. `FouladEbooksConfig.h`'s `FOULAD_EBOOKS_URL`
already hardcodes `https://foulad.one/opds` for exactly this reason; this
only matters for a user's own manually-entered OPDS server URL, where
`UrlUtils::ensureProtocol()` still defaults to `http://` for local-server
support.
