# Update 2026-10-31 (later) — SOLVED: the bridge must start security

**Status: working.** The 7610 dials the bridge and browses with Opera once
the bridge requires an authenticated, encrypted link on RFCOMM. That is the
`/security` switch in the web UI (`rfcomm_set_required_security_level(LEVEL_2)`
in `ppp_link_init`, NVS key `btsec`, off by default, applied at boot). Setup
steps are in the README.

## How it was found

gnubox's `Options → Debug → Test comm channel` (`routedump.cpp:414`) opens
`BTCOMM::5` directly, with no connection agent and no TSY, writes
`~mRouter - Are you there?~` and waits 20 s for a reply. It writes its result
to `c:\nokia\ctest.txt`. With the bridge's security off:

```
123456Error = -6005
78 Error = -6005
```

Both the write and the read fail with **-6005**: Symbian's HCI error base
(-6000) minus HCI error 0x05, *Authentication Failure*. On the bridge, the
same moment looks exactly like every failed dial: RFCOMM ch 4 opens
normally, zero data, the phone closes it about 6 s later, and there is **no
security traffic at all** (no link-key request, no authentication, no
encryption change). So the phone fails its own security check locally,
without ever asking the bridge to authenticate. Re-pairing did not change
that.

The same capture had the answer to whether the bridge can encrypt with this
phone: the post-pairing bootstrap connection into the phone's DUN service got
the 7610 to authenticate and encrypt (key size 16). The phone does run
security as a *server*; it just does not start it as a BTCOMM *client*.

With security on, the same test gives the gnubox tutorials' healthy result:

```
123456Error = 0
78 Error = -3
```

and the bridge logs a link-key request, `Authentication complete
status=0x00`, `Encryption change ... enabled=1`, key size 16, then
`link security: level=3 ... key_size=16` at RFCOMM open, followed by the
phone's bytes. The first real dial after that went straight to PPP (no
`CLIENT` login script was sent) and reached `PPP up` 0.4 s after the channel
opened.

## Why it was missed for so long

- The dial failure gave no useful error: the connection agent swallowed
  -6005 and reported "network problem", or even `KErrNone`.
- The bridge never saw a failed security procedure, because none was
  attempted. A trace with *no* security events looks like "security is not
  involved", which is the wrong reading.
- `GW_REQUIRE_ENCRYPTION` was on the experiment branch in July and was
  dropped as "low value" without being run.
- The gnubox tutorials did say it, as a host-side setting ("Secure
  Connections" on Widcomm, "Enable Data Encryption" on BlueSoleil).

## S60 1st Edition needs it off

With security on, an N-Gage QD cannot connect, so the switch defaults to off.
With it off, the same QD connects straight away and browses normally.

A partial log of one attempt with security on (QD `00:0E:6D:D4:B7:25`,
2026-10-31). Debug logging had been switched off shortly before, so there is
no HCI packet trace, only the bridge's own event lines:

| time | event |
|---|---|
| 516.0 s | QD connects |
| 516.2 s | link-key request; the stored key is used, no PIN |
| 516.3 s | `Authentication complete status=0x00` |
| 516.4 s | `Encryption change status=0x00 enabled=1` |
| after | the controller prints `ASSERT_WARN(1 9), in lc_task.c at line 1556` twice |
| 528.4 s | QD authenticates again: link-key request, `Authentication complete status=0x00` |
| 528.5 s | `Command status error=0x0c opcode=0x0c53` (*Command Disallowed*) |
| after | four more `ASSERT_WARN(1 9)` from `lc_task.c:1556` |
| 545 s | security switched off, bridge reboots |

The QD never opened the serial channel: no `incoming RFCOMM` line at all.

What this does and does not show:

- It is **not** a pairing or encryption failure. The QD authenticates with
  the stored key and the link encrypts. Whatever fails comes after that.
- The rejected opcode `0x0c53` is not a command BTstack defines anywhere in
  its tree (`hci_cmd.c`/`hci_cmd.h`), so where it came from is unknown.
- `ASSERT_WARN(... 9), in lc_task.c at line 1556` is from Espressif's
  closed-source controller. The same line also appeared, seven times, just
  before the 2026-08-05 controller reboot recorded in the README's open
  issue, so it may be a weak spot in the controller rather than a QD
  problem. Its meaning can't be read from our side.
- S60v1's gnubox writes `BTCOMM::0` and handles security differently from
  v1.1, which may also matter.

To trace it fully: switch security on, tick *HCI packet trace* and *BTstack
internal log* on `/debug`, capture the serial console, and connect the QD
once. The trace would show what the QD sends after encryption and what
command the controller rejects.

## Also fixed in this session

`bt_reopen_task` (`main/gateway.c`) called `gap_discoverable_control()` from
its own FreeRTOS task. When that ran while the phone was reconnecting, it
raced RFCOMM and lost a PN response, and the phone waited 63 s on a half-open
channel. Now a BTstack run-loop timer (v1.5.1).

Everything below is the history of the investigation, kept for the record.
The theories it rules out (operator CSD, 2G registration, `BTCOMM::0` vs
`::5`, DUN, server-first LCP, the telephony pre-call) stay ruled out.


# Update 2026-10-31 — 7610 never reaches the bridge; cause still unknown

**Status: superseded by the update above.** An earlier version of this section (shipped
with v1.5.0) named the cause as "PHONETSY places a real CSD call that the
operator refuses". Later tests in the same session disproved that, see below.

Session facts (7610 BT address `00:1B:EE:E1:3D:06`, master firmware with the
full HCI trace from `/debug`):

- Every dial, from Opera or from gnubox `Options → Debug → bring IF up`,
  fails in **2-3 s** ("network problem", or the access-point picker
  reappearing in a loop). The bridge receives **zero HCI packets** from the
  phone: no paging, no SDP, no RFCOMM. Re-running gnubox (BT/Serial and
  BT/Dialup) and power-cycling the phone change nothing.
- **This started with a re-pair.** Earlier in the session, before the bridge
  was deleted from the phone's paired devices and paired again, dials did
  reach the bridge: an ACL, a fresh PIN pairing on every dial (even though
  the phone was already paired), then a hang-up about 4 s later with no SDP
  or RFCOMM. Since the re-pair, no dial has reached the bridge.
- gnubox `bring IF up` log: `iap ID = 4`, then `RGenericAgent::StartOutgoing`
  returns **KErrNone** ("Connection open") after 2 s, still with nothing on
  the bridge, and then the picker loop starts. So the phone's connection agent
  reports success without any Bluetooth activity.
- gnubox log: `BTCOMM CSY Configured.` with `3076f5e4aa06`, no errors.
- CommDB (`Cdbv3.dat`), record `GnuBoxModem`: `csd.agt`, `PPP`,
  `BTCOMM::5`, `PHONETSY`, `BTCOMM`; ISP description `Direct 2box`. There is
  also a `Default Modem` record (`CSD.agt` / `COMM::0` / TSY `MM` / `ECUART`),
  probably just the database template: no `mm.tsy` exists on the phone.
- gnubox v1.1 beta2 source, `src/gnuboxPhone.cpp` `PHONE_SETTINGS[]`: every
  mode (Direct over BT/Serial, BT/LANAcc, BT/Dialup, BT/Auto, IR, Cable)
  writes `TSYName = PHONETSY`. "BT/Dialup" is a Direct mode that only looks
  up the DUN UUID instead of SPP. The older (S60v1-era) gnubox source blanked
  the TSY in its Dialup mode; v1.1 dropped that mode.
- The 7610 has only `phonetsy.tsy` (Z:), nothing under `C:\System`. It has no
  Offline profile and will not boot without a SIM.

**Disproven: "the operator has no CSD, so PHONETSY's data call is
refused".** Two tests:

1. The N-Gage QD's modem record also uses `PHONETSY` (with `BTCOMM::0` and a
   blank number), and it dials the bridge fine on the 7610's SIM, whose
   operator has no CSD.
2. The 7610 fails exactly the same way, 2-3 s and no packets, with an
   inactive SIM in it.

So the TSY name is not the difference by itself, and the network does not
appear to be involved. The July observation that an unregistered 7610 reached
RFCOMM predates the re-pair, so it does not separate the two either.

**Where to resume:** reset the phone's Bluetooth side completely, with the
HCI trace on: delete the pairing, Bluetooth off, power-cycle; pair again
(PIN `0000`); gnubox `2box → Direct over BT/Serial` → the bridge → **left**
soft key at the encryption prompt; power-cycle again; `bring IF up` once.
Watch for whether the phone pages the bridge at any step.

Bridge side, kept on master: a DUN (0x1103) SDP record on the SPP channel (no
third RFCOMM service), and runtime debug switches on `/debug` (HCI packet
trace, BTstack log, verbose), which replace the `GW_HCI_DUMP` compile flag.

The experiment branches (`s60v2-7610-experiments`, `s60v2-3230-experiments`)
are retired and deleted. What they held, and where it went:

| Branch change | Status |
|---|---|
| `GW_HCI_DUMP` compile flag | Replaced by the `/debug` page on master |
| Link-security log at RFCOMM open | On master, behind *Verbose bridge events* |
| DUN 0x1103 record (own channel 2) | On master, sharing SPP channel 4 instead |
| RFCOMM pools 2 → 4 + asserts | Not needed: master still has two RFCOMM services. The gotcha is recorded below and must come back with any third service |
| Server-initiated LCP silence timer | Dropped: disproven (phone never answered) |
| Remote modem status / RX-drop logging | Dropped: showed nothing wrong on our side |
| `GW_REQUIRE_ENCRYPTION` | Dropped: never run, low value |
| 3230 run plan | Now `S60V2-3230-PLAN.md` on master, not run yet |

Ideas considered but parked until the cause is known: a desktop tool that
edits a copy of `Cdbv3.dat` (feasible: text fields are length-prefixed and
same-length patches are trivial, but only useful if the phone has another
agent or TSY to point at), and a custom "null" TSY for the 7610.


# Update 2026-07-29 — BTCOMM::5 tested and eliminated

**Result: no change.** The 7610 was reconfigured from `2box → Direct over
BT/Auto` (`PortName BTCOMM::0`) to `2box → Direct over BT/Serial`
(`BTCOMM::5`, UUID `ESerialPortProfile` 0x1101, device stored explicitly),
answering the encryption prompt with the **left** soft key. The failure is
byte-for-byte the same shape as before: SDP search for 0x1101 → RFCOMM
channel 4 opens → MSC `0x8d` exchanged both ways → credits granted → **zero
user data** → clean `DISC` 5.99 s after open. `bring IF up` re-prompts for
the access point in a loop; Opera reports "network problem".

So the port-name distinction in the article excerpt below is **not** what was
blocking us. Note also that every previous session ran on Auto/BTCOMM::0 —
which per the excerpt should not work on a post-6600 Nokia at all, yet it
dialled. The likely reason is `bt_bootstrap.c` connecting *into* the phone
after pairing, standing in for the mRouter client that arms the callback
port. Unverified: the decisive evidence would be that session's bootstrap SDP
log, which was not kept.

## New instrumentation and what it showed

The experiment branch now logs the ACL's security state at RFCOMM open. On
this run:

```
I (330281) ppp_link: link security: level=0 authenticated=0 key_size=0
```

and the trace for that ACL contains **no** PIN request, link-key request,
`Authentication complete` or `Encryption change`. No security procedure ran
on the link at all.

Two cautions before reading anything into that:

- **`authenticated=0` is not evidence.** For classic links BTstack's
  `gap_authenticated()` is true only for SSP MITM-protected key types (see
  `gap_authenticated_for_link_key_type` in `hci.c`). A legacy PIN-paired
  phone like the 7610 produces a plain combination key and reads false even
  on a link that *was* authenticated. Trust `level` and `key_size`.
- **Absence of security is probably benign, not a broken config.** gnubox
  always calls either `SetEncryption()` or `SetAuthentication()`
  (`gnuboxContainer.cpp:1319-1372`), so a port with no requirement looks
  wrong — but Symbian's BT security manager can satisfy "authentication
  required" from an existing trusted bond without re-authenticating a fresh
  ACL. Do not conclude the Bluetooth registry write failed without the
  gnubox log.

Worth knowing for any future check: on FP1 `NOBTREGISTRY` is not defined
(it is FP2-only, `gnuboxPhone.h:58-60`), so gnubox stores BTCOMM::5's
security level in the phone's **Bluetooth registry** via
`CBTRegistry::SetDefaultCommPort` — *never* in CommsDB. A `commsdb.txt` dump
can confirm `PortName` but can never show the security setting; the ESP32's
HCI trace is the only observable.

## Where this leaves the investigation

The stall is still above RFCOMM, exactly where the 2026-07-24 session
localized it, and the telephony pre-call hypothesis (item 1 below) is now the
sole live one. Forcing encryption from the ESP32 side
(`GW_REQUIRE_ENCRYPTION` on the experiment branch) tests whether *we* can
encrypt, not whether the phone's CSY wanted encryption — low value, left off.

Remaining leads, cheapest first:

1. **2G registration.** Unchanged and still decisive. Independent of
   everything tested here.
2. **The gnubox log** (`c:\logs\gnubox\gnubox.txt`, create the folder first).
   `SetupCSYL`'s `FLOG` breadcrumbs and `LogLeaveIfErrorL(err,"Setting BT
   registry")` would say whether `SetDefaultCommPort` errored. Free, no
   reflash.
3. **Carrier-detect edge (untested, ESP32 side).** A real modem asserts DCD
   only once the call connects; BTstack asserts `0x8d` (incl. DV) during
   channel setup, before the app sees `RFCOMM_EVENT_CHANNEL_OPENED`, so the
   phone never sees a 0→1 transition. `rfcomm_send_modem_status()` could
   drop then re-assert DV. Weak: only produces a 1→0→1 sequence, which
   Symbian may read as a dropped call, and MSC sits *below* the layer where
   the failure was localized.

The article excerpt that prompted this experiment, kept for reference:

-------- BEGIN EXCERPT ---------
You will probably notice that the PortName will show 'BTCOMM::0' for Bluetooth Auto, while 'BTCOMM::5' for any other Bluetooth options. This is the key difference.

The mRouter client software in your phone creates a bluetooth serial port service in the phone. This is used to initate PC suite connection to your phone. If anybody connects to this service using bluetooth, then the connection gets terminated instantly (this is normal), the mRouter client software sets up BTCOMM::0 port to 'callback' to whatever bluetooth device and service just connected. Then it opens BTCOMM::0 thus effectively calling back.

Thus if we use BTCOMM::0 then we do not need to setup the bluetooth communictions. This is easier and this is why it asks no questions about device or encryption. However, the config will be changed by mRouter upon an incoming bluetooth connection to the serial port service! That will be source of problems. Thus using the manual device, service and encryption selection (as required for 'BTCOMM::5') gives more stable results, while Auto selection is quicker but can go wrong time by time. Although it is fairly easy to fix by initiating a PC suite connection (once you recognize that it has gone wrong).

To migrate from Auto to the fixed settings, you should probably choose 2box Bluetooth->Serial port, select your computer and set encryption properly.

Note for later Nokia phones: Nokia has dropped the mRouter based PC suite connection method somewhere after the 6600. If you have a newer phone, then you probably do not even have a Serial port on your phone. This means there is nothing to connect to that would setup BTCOMM::0 to call back to your computer. Thus you can not use the 2box bluetooth->Auto method to connect. You always have to select from the other three 2box BT options and specify the device by hand.
-------- END EXCERPT ---------

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

- Firmware: master. Turn on *HCI packet trace* on the `/debug` page for full
  traces (the experiment branch these notes first referred to is retired).
- Phone: gnubox v1.1 beta (`gnubox_7610.sis`), AP `Bt` per Schuon
  tutorial, login script set, dial-up number blank after 2box.
- Re-pair after every ESP32 reboot (bootstrap is once-per-boot).
- Capture monitor output via `tee` for later analysis.
