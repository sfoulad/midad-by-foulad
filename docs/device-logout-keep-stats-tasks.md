# Device logout: remove the device, keep the reading stats

Implementation handoff, written 2026-07-26. Two repos, **server first** — the
firmware change is unsafe to ship before the server behaviour exists, because
the endpoint it would call today destroys the stats.

## The requirement

1. Logging out on the device removes it from Foulad One.
2. Its reading stats survive the removal.
3. Logout completes **only after the server confirms**. No confirmation, no logout.
4. **No WiFi means no logout.** The device stays signed in rather than diverging
   from the server.

## Why the firmware cannot do this alone

Today `SettingsActivity`'s logout is purely local: it drops the stored OPDS
credential and makes no network call at all. The device row and its token stay
alive on the server, which is the reported bug.

The obvious fix — call the existing `DELETE /api/app/devices/{id}` — is wrong.
Revocation on the server is driven by *deletion*: `Device::booted()` hooks
`deleted` and revokes the device's tokens plus writes a `RevokedDevice`
tombstone. That part is correct and needs no change. But the delete is a real
delete, and `device_reading_stats.device_id` is `cascadeOnDelete()`, so the
history goes with it. Requirement 2 fails.

So the server has to gain a removal that revokes without destroying.

---

# PART A — foulad-ebooks (do this first)

The approach: soft-delete the `Device` row. Laravel fires `deleted` for
soft-deletes, so the existing revocation hook keeps working untouched; the row
survives, so no FK cascade runs and the stats stay.

### A1. New migration — add `deleted_at` to `devices`

`database/migrations/2026_07_26_000001_add_soft_deletes_to_devices_table.php` (new)

`$table->softDeletes();` up, `$table->dropSoftDeletes();` down. Follow the
existing filename convention (see `2026_07_25_000004_create_revoked_devices_table.php`).

### A2. `app/Models/Device.php`

Add `use Illuminate\Database\Eloquent\SoftDeletes;` and the `SoftDeletes` trait.

**Do not touch `booted()` (lines 79–92).** It already revokes tokens and writes
the tombstone, and Laravel fires `deleted` on soft-delete, so it keeps working
as-is. This is the whole reason soft-delete is the right shape here.

### A3. `app/Http/Controllers/OpdsDeviceController.php` — `register()` (from line 35)

**This is the one that will bite in production if missed.**
`devices.serial_number` is **globally unique**
(`2026_07_24_000000_create_devices_table.php` line 22). A soft-deleted row keeps
occupying that serial, but the default query scope hides it — so the upsert finds
nothing, tries to INSERT, and dies on the unique index.

It does not surface immediately, because the `RevokedDevice` tombstone rejects a
removed device's re-registration first. It surfaces the moment the user
**re-pairs over QR**, which lifts the tombstone: the next check-in then hits the
unique constraint and re-pairing fails.

Fix: look the row up `withTrashed()` and `restore()` it. As a bonus this makes a
remove → re-pair cycle keep the device's history rather than starting from zero.

### A4. `app/Http/Controllers/ReaderDashboardController.php` line 21

`$request->user()->devices()->get()` → needs `withTrashed()`.

Without this the dashboard silently loses a removed device's history, which
defeats requirement 2 from the user's point of view even though the rows are
still in the database. Note the dashboard reads `reading_stats_snapshot` (a JSON
column on the device row) as well as the `device_reading_stats` table — both are
preserved by soft-delete, but both are invisible without `withTrashed()`.

### A5. Deliberately leave alone

- `app/Http/Controllers/Api/App/DeviceController.php` line 24 (`index`)
- `app/Http/Controllers/DeviceSettingsController.php` line 25

Both should keep hiding removed devices, which the default soft-delete scope
does for free. No change wanted.

### A6. Decide: is delete idempotent?

Route-model binding for `{device}` excludes trashed rows once `SoftDeletes` is
on, so a second `DELETE` on an already-removed device returns 404. The firmware
should treat 404 as "already gone, success" (see B4) — but confirm that is the
intended contract rather than leaving it implied.

### A7. `tests/Feature/Api/DeviceRevocationTest.php`

Extend rather than replace — the existing "Task 3: the password-path gap" cases
still apply. Add:

- delete → `device_reading_stats` rows for that device still exist
- delete → tokens revoked and `RevokedDevice` written (guards the `booted()` hook
  against a future refactor that swaps soft-delete back out)
- delete → device absent from `GET /api/app/devices`
- delete → its history still counted by the dashboard
- delete → re-pair over QR → `register()` succeeds and the old stats are still attached

---

# PART B — foulad-eink (only after Part A is deployed)

### B1. `src/network/HttpDownloader.h` / `.cpp`

**No DELETE support exists.** The public API is GET (`fetchUrl`,
`downloadToFile`), `postJson` and `postFileMultipart` only. Add a
`deleteRequest()` using `HTTP_METHOD_DELETE`.

Model it on `runPostJson` (from line ~498 in `.cpp`), which already has the right
shape: open, read the body, map status. Two things to carry over deliberately —
accept any **2xx** (see the 201 fix in `2ae037be`; a delete may answer 204), and
treat **404 as success** per A6.

`setDeviceSerialHeader` is already called at every request-setup site and gates
on host, so the new path gets `X-Device-Serial` automatically. Nothing to do.

### B2. `src/FouladEbooksConfig.h`

Add the app-API base, e.g.
`constexpr char FOULAD_EBOOKS_APP_DEVICES_URL[] = "http://foulad.one/api/app/devices";`

Same `http://` and same host as the existing constants, so `isFouladEbooksUrl()`
already matches it and the serial header is sent.

Worth knowing: `/api/app/*` is behind the **same `opds.auth` Basic-Auth
middleware** as the OPDS routes, so the credential the device already holds
works. Verified live: `GET /api/app/devices` with device Basic Auth returns 200
and includes both `id` and `serial_number`.

### B3. New `src/FouladDeviceLogout.h` / `.cpp`

Two steps, because the delete needs the device's server-side `id` and the device
only knows its own serial:

1. `GET /api/app/devices` → parse with ArduinoJson (not a substring scan — see
   the escaped-slash note in `0d18b8fc`) → find the entry whose `serial_number`
   equals `FouladDeviceTracking::getSerialNumber()`.
2. `DELETE /api/app/devices/{id}`.

Return a tri-state, not a bool: **confirmed removed** / **failed** / **not found
on server** (already gone → treat as confirmed). Requirement 3 means only the
first and third may clear the credential.

Heap note: the list response carries every device with full stats blobs — the
live test account returned 6 devices with 15 fields each. Do not slurp it into a
`JsonDocument` unbounded on a 380KB part. Use a filter (`DeserializationOption::Filter`)
for `id` + `serial_number` only, or stream it.

### B4. `src/activities/settings/SettingsActivity.cpp` lines 417–434

The `FouladEbooksLogout` handler. Current body removes the server from
`OPDS_STORE` unconditionally; that unconditional removal is the bug.

New flow, after the existing confirmation dialog:

- If `WiFi.status() != WL_CONNECTED` → **abort the logout**, tell the user WiFi is
  required, leave the credential in place (requirement 4).
- Otherwise call B3, showing progress — this is two round trips, so it must not
  look frozen.
- On **confirmed removed** (or not-found) → `OPDS_STORE.removeServer(i)` and
  rebuild the list.
- On **failure** → leave the credential in place and say so. Never clear locally
  on a failed call; a device that thinks it is signed out while the server still
  lists it is exactly the reported bug in reverse.

Do not run the network calls inline in the input handler — it will block the UI
for seconds. Follow `FouladQrLoginActivity`'s pattern (a state + `loop()`-driven
progression) or push a dedicated progress activity.

### B5. `lib/I18n/translations/english.yaml`

New `tr()` strings — all user-facing text must use `tr()`:

- `STR_LOGOUT_NEEDS_WIFI` — "Connect to WiFi to sign out."
- `STR_LOGOUT_IN_PROGRESS` — "Signing out..."
- `STR_LOGOUT_FAILED` — "Could not sign out. Try again."

Then `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`. Commit the
YAML only; the three generated files are gitignored.

Keep them short, or wrap them: use `drawCenteredTextWrapped` (added in `7f02b5c`),
not `drawCenteredText`, which overruns the panel and clips at both edges.

---

## Verification

Server, once deployed:

```bash
# Pair a test device, note its id and serial from:
curl -s -u 'USER:TOKEN' http://foulad.one/api/app/devices

# Remove it
curl -s -X DELETE -u 'USER:TOKEN' http://foulad.one/api/app/devices/{id}

# 1. Access is cut
curl -s -o /dev/null -w '%{http_code}\n' -u 'USER:TOKEN' \
     -H 'X-Device-Serial: SERIAL' http://foulad.one/opds     # expect 401

# 2. Stats survived  -> check device_reading_stats rows for that device_id
#    and that the dashboard total is unchanged

# 3. Re-pair over QR, then check the history is still attached  <-- A3
```

Device: sign out with WiFi off (must refuse), then with WiFi on (must disappear
from Foulad One while the dashboard total holds), then re-pair.
