#pragma once

// Planned Midad Core integration target -- NOT wired into any code path yet.
//
// "Midad Core" (midad-core-by-foulad) is the Cloudflare-native backend that will
// eventually replace the legacy Laravel "Foulad eBooks" server this device talks to
// today via FouladEbooksConfig.h. This file exists so the intended target host/URLs
// are recorded in one place ahead of the actual client work, per
// docs/core-integration-readiness.md -- it is deliberately not `#include`d anywhere,
// so it has zero effect on any build until a real integration PR wires it up.
//
// Do not point any live request at these constants without first reading
// docs/core-integration-readiness.md: as of this writing, Core has no Bearer-token
// acquisition path this device can use (it only ever speaks HTTP Basic Auth / a
// QR-issued device token, and Core's OPDS surface requires a Bearer JWT for the one
// endpoint that matters -- book download), and this device's TLS posture for the
// OPDS/download path (wolfSSL via SecureHttpClient, see HttpDownloader.cpp's
// runGetWolf()) calls setInsecure() unconditionally today, so switching that path to
// https:// here would encrypt the connection without authenticating the server.
// Neither gap is closed by adding these constants.

// Core's permanent API/OPDS hostname (Cloudflare Worker, `apps/api` in
// midad-core-by-foulad). Live and already serving the real Round 8 OPDS surface as
// of this writing (verified: `curl -I https://api.midad.one/opds` -> 200
// `application/atom+xml;profile=opds-catalog;kind=navigation`) -- but "reachable"
// is not "ready to switch to," see the caveats above.
constexpr char MIDAD_CORE_HOST[] = "api.midad.one";

// Root OPDS navigation feed. Mounted at `/opds` in midad-core-by-foulad's
// apps/api/src/index.ts (note: no trailing slash -- `/opds/` 301-redirects to
// `/opds`, a Hono mounted-sub-app quirk, confirmed live).
constexpr char MIDAD_CORE_OPDS_URL[] = "https://api.midad.one/opds";

// Contract differences from the legacy FOULAD_EBOOKS_* endpoints this device
// currently uses, all confirmed by reading midad-core-by-foulad's
// apps/api/src/opds/routes.ts (Round 8) directly:
//
//  - Auth: legacy protects the whole OPDS surface with HTTP Basic Auth (see
//    OpdsServerStore's username/password). Core has no Basic Auth at all --
//    browsing (root/category/books/search) is public, and only
//    `GET /opds/books/{id}/download` requires `Authorization: Bearer <JWT>`
//    (401 without it). This device has no code path that produces a Bearer
//    token today.
//  - Acquisition format: every acquisition `<link>` Core emits is
//    `type="application/epub+zip"` -- Core's ingestion pipeline has no PDF/XTC
//    conversion step, so it never emits `application/x-xtc` or
//    `application/x-xtch`. An XTC-only legacy book has no Core equivalent yet.
//  - Download URL shape: `/opds/books/{id}/download` 302-redirects to a
//    short-lived (300s) SigV4-presigned R2 URL, not a Laravel `?signature=&user=`
//    link -- but this device already treats acquisition/cover hrefs as opaque
//    (see docs/opds-server-reference.md), so the shape change itself needs no
//    client-side URL-construction change.
