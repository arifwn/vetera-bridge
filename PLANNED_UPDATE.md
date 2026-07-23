# Planned update: ESP32-initiated bootstrap connection

> **Status: implemented and validated on hardware** — see
> `main/bt_bootstrap.c`. The SDP probe + one-shot connect worked as
> theorized; after the bootstrap the phone's gnubox dial reached the ESP32.
> Kept for the reasoning record.

## Problem this is trying to solve

Phone-initiated connections work at the pairing level (`GAP_EVENT_PAIRING_COMPLETE`
+ link key notification confirmed working), but gnubox's redirect never
produces any outbound RFCOMM connection from the phone to the ESP32 — the
serial log shows zero HCI activity when browsing via the `Bt` access point,
even with a valid bond in place. This matches the README's documented
theory: some gnubox builds never write the phone's default BT comm port
unless something has connected *into* the phone's Bluetooth serial service
at least once (the "mRouter" registration).

Separately: connecting from a PC to the phone over Bluetooth serial holds
the connection stably (until manually disconnected) — unlike the phone's
own connection attempts into the ESP32, which tear down within seconds
(that teardown is now understood to be normal post-pairing behavior, not a
symptom of the same problem).

## What this update is — and isn't

**Is:** a one-shot bootstrap. After the ESP32 finishes pairing with a
phone, it opens an SDP query + RFCOMM connection *into* the phone's own
Bluetooth serial service, holds it briefly, then disconnects — mimicking
what the PC did. The goal is purely to register the ESP32 as the phone's
default BT comm port, exactly as the README's manual workaround describes,
just automated instead of requiring a second physical device.

**Isn't:** a new data path. The bridge's actual PPP traffic must still flow
with the **phone as the connecting/client side and the ESP32 as the RFCOMM
server** (LAP ch 3 / SPP ch 4, as today) — that direction is correct for
Bluetooth DUN and is not changing. If the ESP32 connects into the phone's
serial service, it most likely reaches the phone's own AT/modem interface
(phone acting as a modem for a PC), which is the wrong direction to carry
the phone's outbound PPP session. This update does not touch
`ppp_link.c`'s server model at all.

## Open question that gates whether this is viable

We don't yet know **what service the PC's connection actually reached** on
the phone, or whether the ESP32 can discover/reach the same service via
SDP. Need from you before implementation starts:

- When you connected from the PC to the phone over Bluetooth, what did you
  connect to specifically — a COM port assigned by Windows/the PC's BT
  stack? A serial terminal app? Do you know which BT profile/service that
  was (SPP, DUN, something else)?

## Recommended free test before writing any firmware (do this now, zero cost)

Per the mRouter theory, an incoming connection registers *the connecting
device* as the phone's default comm port. Your PC just connected in and
held the link. Before we build anything: **retry the gnubox dial right
now** (`2box Direct → Bluetooth → Vetera Bridge`, browse to
`192.168.7.1`). If the phone now emits an outbound RFCOMM attempt to the
ESP32, the whole theory is confirmed and this firmware change just
automates what your PC did — quicker to prove than to build. If it's still
silent, the theory weakens and the plan below needs to be reconsidered
before implementation.

## Implementation plan (pending confirmation of the above)

1. **SDP probe (investigation, not yet the bootstrap logic itself).** From
   the ESP32, after `GAP_EVENT_PAIRING_COMPLETE` fires for a given
   BD_ADDR (we already have it, e.g. `00:60:57:AB:51:97` in past logs),
   run an SDP service search against that address for RFCOMM/serial-port
   services. Log whatever comes back. This tells us whether the phone
   exposes anything connectable at all, and on which channel — the
   question the whole plan depends on.
2. **One-shot connect-and-drop**, gated behind the SDP probe finding a
   channel: `rfcomm_create_channel()` to that channel, wait for
   `RFCOMM_EVENT_CHANNEL_OPENED`, then immediately
   `rfcomm_disconnect()`. No data is sent or read on this link — its only
   purpose is the act of connecting.
3. **Trigger condition:** run this once per newly-paired BD_ADDR (track
   with a small "already bootstrapped" flag/set, not persisted — a fresh
   pairing is a rare, deliberate user action). Do not retry on a timer
   and do not run this for already-known/previously-bonded addresses on
   every boot.
4. **No new subsystem beyond that** — no persistent client state, no
   retry/backoff logic, no changes to `ppp_link.c`'s server path.

## Risks / things to watch for if this proceeds

- If the SDP probe finds nothing connectable, the plan stops at step 1 —
  there is nothing to automate.
- If the "service" the ESP32 reaches is actually the phone's AT/modem
  interface, connecting into it and then abruptly disconnecting might
  leave the phone's modem stack in a bad state for a moment. Worth testing
  whether this has any visible side effect on the phone.
- This adds a small amount of one-time BTstack/RFCOMM client code
  (`l2cap`/`rfcomm` client APIs are already linked in since the server
  side uses them) — should be a minor flash/IRAM cost, but the README
  notes IRAM is already tight (`-Os` is load-bearing); worth confirming
  the build still fits after this is added.
