# Foulad eBooks — let a device sign itself out

Spec for the **foulad-ebooks** server. Written 2026-07-26, after testing the
deployed soft-delete work against production.

One endpoint. Everything else it depends on already exists and works.

## Why

Signing out on the device now removes it from the account
(`docs/device-logout-keep-stats-tasks.md`, shipped in `v1.7.89-rc`). It does that
by calling `GET /api/app/devices` then `DELETE /api/app/devices/{id}`.

That works only for installs that signed in with the **account password**. Every
QR-paired device holds a **device token**, and the app API is behind
`RejectDeviceTokenAuth`, which rejects tokens with a 403:

> A device token is a convenience credential that lives on an e-ink reader and
> travels over plain HTTP. The app API can delete devices, change settings and
> manage fonts, so a token lifted off a device must not reach it.

That reasoning is right and the middleware should stay. The problem is the shape
of the request, not the control: the firmware does not need "delete any device by
id" — it needs "remove *me*". That is a far narrower capability, and it is safe
to expose to a token in a way the app API is not.

Confirmed against production: `GET /api/app/devices` with a QR-issued token
returns `403 {"message":"This endpoint requires an account password, not a device
token."}`, while the same call with the account password returns 200. So QR
devices — the primary sign-in path — currently cannot sign themselves out at all.

## The endpoint

```
POST /opds/device/signout
```

On the **OPDS** surface (`routes/opds.php`), behind `opds.auth` and deliberately
**not** behind `RejectDeviceTokenAuth` — reaching it with a device token is the
entire point.

Request:

```json
{ "serial_number": "XTE-A1B2C3D4" }
```

Behaviour: find the authenticated user's device with that serial and remove it,
by exactly the same path the app API's delete uses — `$device->delete()`, now a
soft-delete. `Device::booted()`'s `deleted` hook then revokes the tokens and
writes the `RevokedDevice` tombstone as it already does. **No new removal logic,
and no second code path that could drift from the first.**

Responses:

| Status | Meaning | Firmware does |
|---|---|---|
| `200` | Removed | clears the stored credential |
| `404` | No such device for this user | clears it too — the end state already holds |
| `403` | Serial does not match the caller (see below) | keeps the credential, reports failure |
| `401` | Bad credential | keeps it |

## The security property that makes this safe

**A device token must only be able to sign out its own device.**

Without that, a token lifted off one reader could remove any other device on the
account — which is precisely the escalation `RejectDeviceTokenAuth` exists to
prevent, reintroduced through a side door.

The pieces are already there. `OpdsBasicAuth` sets `auth_via` and
`device_token_id` (lines ~96-97), and `device_tokens` carries `serial_number`
(migration `2026_07_25_000003`). So:

- if `auth_via === 'device_token'`, require the token's own `serial_number` to
  equal the request's `serial_number`, and `403` otherwise;
- if authenticating with the account password, allow any serial the user owns —
  same authority the app API already grants.

Please cover the mismatch case in a test. It is the one that matters and the one
that will look like it works if it is missing.

## Not in scope

Nothing else changes. Soft-delete, the tombstone, token revocation, the
`X-Device-Serial` 401 path and the QR re-pair restore are all deployed and were
verified end-to-end on production before this was written:

- register throwaway device -> `device_id: 17`
- delete it -> gone from `GET /api/app/devices`
- `GET /opds` with `X-Device-Serial` for that serial -> **401**
- `POST /opds/device` for it -> **403**, "This device was removed from your account."
- QR re-pair -> lifts the tombstone
- re-register -> **`device_id: 17` again**

That last line is the proof soft-delete is live: the row was restored rather than
recreated, so `cascadeOnDelete` never fired and the reading stats survived.

## Verifying

```bash
# As a QR-issued device token, sign out that device.
curl -s -o /dev/null -w '%{http_code}\n' -u 'USER:DEVICE_TOKEN' \
     -X POST http://foulad.one/opds/device/signout \
     -H 'Content-Type: application/json' \
     -d '{"serial_number":"ITS_OWN_SERIAL"}'          # expect 200

# The same token must NOT be able to remove a different device.
curl -s -o /dev/null -w '%{http_code}\n' -u 'USER:DEVICE_TOKEN' \
     -X POST http://foulad.one/opds/device/signout \
     -H 'Content-Type: application/json' \
     -d '{"serial_number":"SOME_OTHER_DEVICE"}'       # expect 403

# Access is cut, and the stats survive.
curl -s -o /dev/null -w '%{http_code}\n' -u 'USER:DEVICE_TOKEN' \
     -H 'X-Device-Serial: ITS_OWN_SERIAL' http://foulad.one/opds   # expect 401
```

## Firmware side

Already written and shipping in the same release as this spec. `FouladDeviceLogout`
tries `POST /opds/device/signout` first and falls back to the old
list-then-delete path on **404**, so it works before and after this endpoint is
deployed with no coordination between the two releases:

- **before**: signout 404s, falls back, password logins work, QR devices fail as they do today
- **after**: signout succeeds for both, and the fallback stops being reached

The fallback can be deleted once every install is past that release; it is marked
in the source as removable.
