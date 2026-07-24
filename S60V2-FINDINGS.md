# S60 2nd Edition (Nokia 7610) findings — 2026-07-24 session

Status: **still not working end-to-end, but the failure is now precisely
located**. All experimental firmware changes live on branch
`s60v2-7610-experiments` (commit 7c00eb5); master remains the validated
S60v1 firmware. This document records what was learned so the
investigation can resume without re-deriving anything.

## TL;DR

With the right gnubox build, the 7610 **does dial the ESP32** — SDP
lookup, RFCOMM channel 4 fully opens, modem signals exchange cleanly.
The phone then never passes a single byte of user data and hangs up
after ~6 s. HCI-level tracing proves the ESP32 side is correct through
the entire RFCOMM layer. The stall is in the phone's *telephony* layer:
Symbian's dial agent asks the GSM telephony server (`PHONETSY`) to
complete the "data call" before it will use the open Bluetooth port, and
the leading hypothesis is that this pre-call check fails because the
phone has **a SIM but no 2G network registration** (every historically
working 7610+gnubox setup, circa 2005, sat on a live GSM network).

## The gnubox build matters (this was the original "never dials" bug)

- The earlier attempt used a **6600-targeted gnubox** — that is Mika
  Raento's older fork for base S60 2nd Ed (Symbian 7.0). The 7610 is
  **FP1 (Symbian 7.0s)** — a different comms architecture.
- The correct build is **-xan-'s gnubox v1.1 beta** (`gnubox_7610.sis`,
  shared with the 6670). Key difference found in its source
  (`gnuboxPhone.h` / `gnuboxContainer.cpp`): on `__SY70s__`/`__SY80__`
  it writes `MODEM_AGENT = csd.agt` and `IF_NAME = PPP` into the CommDB
  modem record — fields the 6600 build never writes. Without them FP1's
  connection agent has nothing to invoke, which matched the old
  "CommDB looks right but nothing dials" symptom exactly.
- Archived copies (Wayback Machine, snapshot 20080125044214 of
  `http://gnubox.dnsalias.org/gnubox/`):
  - `gnubox_7610.sis` (30 921 bytes, resolves to capture 20070623193228)
  - `gnubox_v11beta2.tar.gz` source
  - `gnubox7610schuontut.pdf` — Schuon's 7610/6670 setup tutorial
    (access point `Bt`, bearer *Data call*, the CLIENT/SERVER login
    script, Windows-dialin host setup)
- Phone-side setup per that tutorial was completed successfully,
  including the login script and a **blanked dial-up number** (verified
  — the "still dials 0000" theory was checked and eliminated).

## What the HCI traces proved (diagnostic build, GW_HCI_DUMP)

Dial sequence observed from the phone, all correct:

1. SDP ServiceSearch for **SerialPort 0x1101 only** — it never asks for
   DUN (0x1103) or LAP (0x1102). The "ESP32 must advertise a DUN
   gateway" hypothesis from the README is **disproven** for the
   dial-time lookup.
2. Reads our SPP record, resolves RFCOMM channel 4.
3. RFCOMM mux SABM #0 → UA, PN for DLCI 8 (credit-based flow control),
   SABM #8 → UA, MSC exchange both directions — phone asserts
   `0x8d [RTC RTR DV]` (ready + carrier), we assert the same.
4. Channel open, credits granted both ways.
5. **Zero user data from the phone, ever.** No login-script `CLIENT`,
   no AT commands, no LCP.
6. When the ESP32 sends LCP ConfReq first (see experiments below), the
   phone's RFCOMM layer *acknowledges receipt* (grants fresh credits)
   but nothing above it ever answers — proof our bytes reach the phone
   and die above its Bluetooth serial driver.
7. ~6 s after open: clean `DISC` from the phone, then L2CAP/ACL
   teardown. A deliberate hang-up, not a crash.

Interpretation: Symbian FP1's CSD dial sequence is
*(open CSY port → TSY completes "call" → login script → PPP)*. Steps
3-4 never start, so the failure is inside step 2 — the `PHONETSY`
pre-call — which does not touch Bluetooth at all. With a SIM present
but **no 2G network**, the telephony server refusing to complete even a
blank-number data call after ~6 s fits every observation.

## Experiments run from the ESP32 side (branch `s60v2-7610-experiments`)

| Experiment | Result |
|---|---|
| Server-initiated LCP (Windows-RAS style): silence timer fires 1.5 s after RFCOMM open, PPP restarts as initiator via `ppp_set_passive` + `pppapi_connect` | Phone ACKs the frames at RFCOMM level, never replies. Disproves "FP1 PPP waits for server to speak first" |
| DUN gateway (0x1103) SDP record on RFCOMM ch 2 | Phone never searches for it at dial time |
| Remote modem status logging | Phone asserts full ready+carrier (`0x8d`) — no V.24-level stall |
| RX-drop logging | Nothing was ever dropped on our side |

## Firmware gotcha discovered (worth keeping even on master)

`btstack_config.h` sets `MAX_NR_RFCOMM_SERVICES 2`. Registering a third
RFCOMM service makes the *last* `rfcomm_register_service` fail
**silently**, and BTstack then answers the phone's PN/SABM for that
channel with DM (refusal) *below* the app packet handler — completely
invisible in app-level logs. This cost a full debug round (it looked
like the phone regressing). The branch raises the pools to 4 and adds
`btstack_assert` on every registration. If master ever grows a third
service, this must come with it.

Also identified while reading logs: `[HCI] event 0x6e` is
`HCI_EVENT_TRANSPORT_PACKET_SENT` and `0x66` is
`BTSTACK_EVENT_SCAN_MODE_CHANGED` — both routine noise. Worth
suppressing in `gateway.c`'s default log case someday.

## Where to resume

1. **Get 2G registration and simply retry.** A SIM on a carrier with
   live 2G (or a location with coverage) is the single cheapest
   decisive test. If the dial completes once registered, S60v2 support
   reduces to "requires GSM network" — document it and done.
2. If network registration is unavailable: capture the **phone-side
   error**. When the dial fails, Symbian shows an error note — its
   exact text (e.g. "No network coverage" vs "Connection failed")
   distinguishes the telephony pre-call hypothesis from a config
   problem. GnuBox's `Options → Debug → bring IF up` triggers the dial
   with better feedback than Opera.
3. Still-unexamined evidence: CommsDB dump
   (`Options → Debug → Dump Full CommsDB` → `c:\nokia\commsdb.txt`) and
   the gnubox event log (create folder `c:\logs\gnubox` first; log
   appears as `gnubox.txt` inside). Diff the MODEM/ISP records against
   what `gnuboxContainer.cpp` writes for `__SY70s__`.
4. Long-shot alternative if the network requirement is hard-blocking:
   the mRouter route. S60v2 ships Nokia PC Suite's mRouter client,
   which needs no cellular network — but the ESP32 would have to speak
   Intuwave's proprietary mRouter protocol (the "wsockhost.mrouter"
   proxy method in the gnubox docs). Significant reverse-engineering;
   only worth it if 2G is truly unobtainable.

## Re-testing checklist (7610)

- Firmware: branch `s60v2-7610-experiments` (has diagnostics; set
  `GW_HCI_DUMP` to 0 in `gateway.c` unless full traces are needed).
- Phone: gnubox v1.1 beta (`gnubox_7610.sis`), AP `Bt` per Schuon
  tutorial, login script set, dial-up number blank after 2box.
- Re-pair after every ESP32 reboot (bootstrap is once-per-boot).
- Capture monitor output via `tee` for later analysis.
