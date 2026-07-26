# Foulad eBooks — removal stopped sticking for password-login readers

For the **foulad-ebooks** team. Written 2026-07-27 after re-testing production
following the "the phone can now recover on its own" change.

Not a bug report against that change — for the phone it is clearly right. It has
a side effect on e-ink readers that looks unintended.

## What changed

Re-registering a removed device with the **account password** now succeeds where
it was refused earlier the same day. Measured on production, same account, same
endpoint, hours apart:

| | Before | Now |
|---|---|---|
| `POST /opds/device`, removed serial, account password | `403` "This device was removed from your account." | `200`, row restored with its history |
| `POST /opds/device`, removed serial, **QR device token** | `401` | `401` (unchanged) |
| `GET /opds` + `X-Device-Serial`, removed, before re-register | `401` | `401` (unchanged) |

So the tombstone is no longer credential-agnostic: an account password lifts it,
a device token does not.

## Why it lands differently on a reader

The firmware calls `registerDevice()` when it connects to the catalog and again
on a timer, **before** it fetches any feed
(`OpdsBookBrowserActivity.cpp:1003` and `:164`). On a removed password-login
reader the sequence is now:

1. `POST /opds/device` → `200`, the row is restored
2. feed fetch → `200`, because it is no longer removed

The `401` that would have triggered the device's own sign-out (`ff75aef3`) never
fires. The reader silently comes back, and the person who removed it sees it
reappear in "My Devices" with no action on their part.

Before this change that same reader stayed out: re-registration was refused, and
the device kept only read access until the password changed. Now the row returns
too.

## What this is and is not

**It is not a new hole against someone holding the account password.** It never
was: `EINK_OPDS_SERIAL_HEADER_TASKS.md` says so plainly —

> It is not a defence against someone who holds the account password and simply
> omits the header; only changing the password fixes that.

Anyone with the password can sign in on any device regardless. That is unchanged.

**What it does break is the cooperative guarantee**, which is the case the whole
mechanism was built for: a reader that honestly identifies itself used to stay
removed, and now does not. "I sold this device, cut it off" stops holding for
every pre-QR install — the exact scenario the serial header was added to cover.

QR-paired readers are unaffected and were verified again: token revoked, `/opds`
`401`, re-register `401`. Removal sticks completely there.

## Options

**1. Gate revival on device class (recommended).** Let a phone recover, keep the
tombstone credential-agnostic for readers. The two are cleanly distinguishable
today — readers register as `XTE-<MAC>` with model `Xteink X3`/`Xteink X4`
(`FouladDeviceTracking::getSerialNumber()` builds that prefix and it is stable),
phones as `FOULAD-ONE-*` with `iPhone …`/`foulad-one`. Recording the device kind
on the `revoked_devices` row at removal time would be sturdier than sniffing the
serial at re-registration, and costs one column.

**2. Make phone recovery explicit.** Leave `POST /opds/device` strict for
everyone and give the phone a dedicated recover/undo call. Cleanest boundary, but
needs a change in the phone app, which is presumably what the relaxation was
avoiding.

**3. Accept it, and finish the QR migration instead.** Defensible, and it is the
permanent answer either way — once every install holds a revocable token this
whole class of problem disappears, as the QR results above show. The question is
what fraction of live installs are still on manual password login. We do not have
that number on the firmware side; if it is small and shrinking, option 3 is
reasonable and this document is just a note to revisit. If it is not, option 1 is
cheap.

## No firmware change would help

Worth stating so nobody spends time on it. The reader cannot decline to be
revived: it does not know it was removed. Its `POST /opds/device` succeeds, so
there is no signal to react to. Suppressing the register call to look for a `401`
first would break device tracking and settings sync for everyone to serve this
one case, and a removed device could still be revived by the timer path.

The server is the only place that can decide whether a removal sticks.

## Reproducing

```bash
SER=XTE-STICKTEST01
# register, note device_id
curl -s -u 'USER:PASSWORD' -X POST http://foulad.one/opds/device \
  -H 'Content-Type: application/json' \
  -d "{\"serial_number\":\"$SER\",\"name\":\"t\",\"model\":\"Xteink X4\",\"firmware_version\":\"1.7.90\"}"
# remove it
curl -s -X DELETE -u 'USER:PASSWORD' http://foulad.one/api/app/devices/{id}
# re-register with the account password -- currently 200, restored
curl -s -u 'USER:PASSWORD' -X POST http://foulad.one/opds/device \
  -H 'Content-Type: application/json' \
  -d "{\"serial_number\":\"$SER\",\"name\":\"t\",\"model\":\"Xteink X4\",\"firmware_version\":\"1.7.90\"}"
```

Same steps with a QR-issued token in place of the password return `401` at the
last step, which is the behaviour readers should keep.
