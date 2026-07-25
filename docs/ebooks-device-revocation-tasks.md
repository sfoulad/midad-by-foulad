# Foulad eBooks — make device removal actually revoke access

Spec for the **foulad-ebooks** server. Written 2026-07-25 by the foulad-eink
session, as the counterpart to that repo's `EINK_QR_LOGIN_TASKS.md`.

The device side is built and shipped. This describes the two server changes it
depends on, neither of which exists yet.

## The problem

`EINK_QR_LOGIN_TASKS.md` already flags this under "Not in this phase":

> No "sign out this device" call from the device itself — revocation is done
> from the phone app or the web. Deleting the device row does not currently
> revoke its token; that's deliberate for now (the device re-registers on its
> next check-in) but easy to change.

So today, removing a device from Foulad One is cosmetic:

1. The device row is deleted.
2. Its token still authenticates against OPDS.
3. On the next check-in `POST /opds/device` upserts by `serial_number`, so the
   row **comes back**.

The user's expectation is the opposite: logging the device out should cut it
off and keep it off.

## What the firmware already does

Shipped in foulad-eink `ff75aef3`. On a **401** from an OPDS feed fetch, the
device:

- stops retrying immediately (no retry loop on a rejected credential);
- deletes the stored credential for that server, matched on url **and**
  username, so other configured OPDS servers are untouched;
- shows "Signed out" and lands on the QR sign-in screen at next launch.

**The 401 is the entire trigger.** The device has no other way to learn it was
revoked — it does not poll for its own status. If the server keeps answering
200, the device stays signed in indefinitely.

## Task 1 — Revoke the token when the device is removed

When a device is deleted (phone app or web), invalidate the token issued to it
by the QR flow.

After this, the next OPDS request from that device gets 401 and the firmware
signs itself out on its own.

Note the current one-token-per-login behaviour, also from the spec:

> One token per successful QR login. Repeating the flow issues an additional
> token rather than replacing the old one.

So a device that paired more than once may hold several valid tokens. Revoking
only the newest would leave it working on an older one. **Revoke every token
associated with that device**, not just the most recent.

## Task 2 — Stop `POST /opds/device` from resurrecting the row

Revoking the token is not sufficient on its own. `/opds/device` upserts by
`serial_number`, so a device that still has *any* working credential — or a
still-valid account password from the pre-QR manual login path — can re-create
the row it was just removed from.

Removal needs to be durable: a re-register attempt from a removed device should
be refused rather than silently recreating it. How you express that (a
tombstone, a revoked-serial list, a flag on the row instead of deleting it) is
your call — the firmware only cares that the request fails.

## Edge case worth deciding explicitly

A device that signed in with the **manual username/password** path holds the
real account password, not a device token. Revoking tokens does nothing to it:
it will keep authenticating and keep re-registering.

Two options:

- accept it for now, and rely on the planned migration of those installs to
  tokens (tracked on the firmware side); or
- have removal also refuse that device by serial, per Task 2.

The second is what actually matches "log this device out". Worth being
deliberate, because right now every pre-QR install is in this category.

## How to verify

```bash
# 1. Pair a device (or simulate one) and capture the issued token
curl -s -X POST http://foulad.one/api/device-login/start \
     -H "Content-Type: application/json" \
     -d '{"device_name":"Revocation Test","serial_number":"XTE-REVOKE01","model":"Xteink X3","firmware_version":"1.7.78"}'
# approve it as the test account, then poll to collect username + token

# 2. Confirm it works
curl -s -o /dev/null -w "%{http_code}\n" -u "USER:TOKEN" http://foulad.one/opds
# expect 200

# 3. Remove the device from the phone app / web

# 4. The same credential must now be rejected  <-- Task 1
curl -s -o /dev/null -w "%{http_code}\n" -u "USER:TOKEN" http://foulad.one/opds
# expect 401

# 5. And it must not be able to re-register  <-- Task 2
curl -s -o /dev/null -w "%{http_code}\n" -u "USER:TOKEN" \
     -X POST http://foulad.one/opds/device \
     -H "Content-Type: application/json" \
     -d '{"serial_number":"XTE-REVOKE01","name":"Revocation Test","model":"Xteink X3","firmware_version":"1.7.78"}'
# expect a failure, and the device must NOT reappear in the device list
```

Step 4 is the one the firmware depends on. Steps 2 and 5 are what stop the
removal from silently undoing itself.

## Unrelated, but noticed while testing the QR flow

- `qr_payload` serialises with escaped forward slashes
  (`https:\/\/foulad.one\/link\/CODE`). Valid JSON, and a real parser handles
  it, but any client that substring-scans that field will encode literal
  backslashes into the QR and every scan fails silently. Worth a line in
  `EINK_QR_LOGIN_TASKS.md`.
- In that spec's "Quick manual check", step 2 (`approve`) uses `https://` while
  steps 1 and 3 use `http://`. Presumably deliberate, since approve sits inside
  the HTTPS-forced group — but it reads like a typo and would trip anyone
  following it verbatim.
- `/admin/api/device-logs` is plain HTTP and takes a long-lived admin bearer
  token, so that token crosses the wire in cleartext on every daily health
  check. Moving that endpoint to HTTPS would be worthwhile independently of any
  of the above.
