# Vetera Bridge

**Bluetooth PPP → WiFi bridge for pre-PAN retro phones, on ESP32**

A sibling of [Satura Bridge](https://github.com/sigildeveloper/satura-bridge),
for the older generation it can't reach. Satura Bridge gives internet to retro
phones over **Bluetooth PAN** — but the oldest smartphones (Symbian 6.1 /
S60 1st Edition: Nokia N-Gage, 7650, 3650...) predate PAN entirely. Vetera
Bridge covers them: the phone's own GSM-data dial-up stack is redirected to a
Bluetooth serial (RFCOMM) link with **gnubox**, and the ESP32 terminates the
resulting raw PPP connection and NATs it out over WiFi.

```
[Phone] --RFCOMM, raw PPP (LAP 0x1102 / SPP 0x1101)--> [ESP32] --WiFi + NAT--> Internet
```

The phone thinks it is making an ordinary GSM data call. No drivers, no proxy
apps on the phone — just gnubox's one-time CommDB edit.

## Features

- **LAP (0x1102) + SPP (0x1101)** SDP records, browse-group listed, so old
  Nokia SDP browsers actually find them; Class of Device reports
  *LAN Access Point* for picky device pickers
- lwIP **PPPoS server**: no auth, no VJ compression, no CCP, no IPv6CP on the
  wire — every option old Symbian PPP stacks choke on is compiled out
- **List of up to 8 WiFi networks**: scans and connects to the strongest
  saved network, with retry/backoff across the whole list
- Web setup UI at **http://192.168.7.1** over the phone's own PPP link
  (plain HTML — renders in Opera for S60v1), with captive-DNS redirect:
  open any http:// page before WiFi is configured and you land in setup
- DNS forwarder with cache; legacy pairing with fixed PIN `0000`
- Watchdogs: WiFi-stuck recovery, DNS-hang restart, low-heap reboot

## Hardware

Original **ESP32** (ESP32-D0WD family) — it must have Bluetooth Classic.
Later chips (S2/S3/C3/C6...) are BLE-only and will not work.
An external antenna helps noticeably: BT and WiFi share one radio.

## Building

Requires Docker only — nothing else installed on the host.

BTstack is not vendored; fetch it once (pinned to **v1.8.2**, the version the
`components/btstack-esp32` port glue was taken from):

```sh
git clone --depth 1 -b v1.8.2 https://github.com/bluekitchen/btstack.git components/btstack
```

Build with the official ESP-IDF image:

```sh
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.4.4 \
    bash -c "idf.py set-target esp32 && idf.py build"
```

Flash (adjust the serial device):

```sh
docker run --rm --device=/dev/ttyUSB0 -v "$PWD":/project -w /project \
    espressif/idf:v5.4.4 idf.py -p /dev/ttyUSB0 flash monitor
```

A locally installed ESP-IDF v5.4.x works the same way without Docker.

## Phone setup (S60 1st Edition, e.g. N-Gage Classic)

1. **Pair**: phone → Connectivity → Bluetooth → Paired devices → New paired
   device → select **Vetera Bridge** → PIN `0000`.
2. **Access point** named exactly `Bt` (gnubox looks it up by name):
   - Data bearer: `GSM data`, Dial-up number: `2222` (gnubox blanks it later)
   - Username `RasUser`, Prompt password `No`, Password `pass`
   - Authentication: `Normal`, not `Secure` — this bridge has no PPP
     authentication support at all (PAP/CHAP compiled out, see Design
     notes), so `Secure` forces a CHAP request the ESP32 can't negotiate
   - Gateway IP: leave blank; Homepage: any plain-http page
3. Install **gnubox** (S60 1st Ed build — Mika Raento's original, see his
   [archived page](https://web.archive.org/web/20080208122023/http://mikie.iki.fi/symbian/bt-ap.html)),
   create `c:\logs\gnubox` on the phone, **back up the phone first** (gnubox
   rewrites CommDB), then run gnubox → `Options → 2box Direct → Bluetooth` →
   pick **Vetera Bridge**.
4. Open the browser (Opera recommended — it speaks plain HTTP, unlike the
   built-in WAP `Services` browser) with access point `Bt` and load
   `http://192.168.7.1`. This works before any WiFi is configured.
   Add your WiFi network(s) on the `/wifi` page.
   An official Opera Mobile build for S60 1st Edition (N-Gage etc.) is
   still available from Opera's FTP archive:
   <https://ftp.opera.com/pub/opera/series60/1.x/620/apac/opera_s60_620_asia_61_10.sis>
5. Load a plain-HTTP page from the internet, e.g. `http://frogfind.com`.
   HTTPS-only sites will fail — that's the phone's ancient TLS, not the link.

If the phone connects to nothing after gnubox, it may not have stored the
picked device: some gnubox builds never write the default BT comm port.
Symptom: the `Bt` access point's Dial-up number stays `2222` instead of
being blanked, and the ESP32's serial log shows zero HCI activity at all
(no connection request, nothing) when you try to browse — the phone isn't
even attempting a Bluetooth connection.

Fix that has worked in practice: in gnubox, switch `Options → 2box Direct`
to `Infrared`, then switch it back to `Bluetooth → Vetera Bridge`. This
re-triggers gnubox's CommDB rewrite and clears the stale dial-up number.
Re-check the `Bt` access point afterward — Dial-up number should now be
blank.

If that doesn't work, the fallback from gnubox's own theory of the bug is
to trigger the phone's mRouter to connect out once (connect to the phone's
serial port from another Bluetooth device) so it stores the bridge as its
default Bluetooth serial device. gnubox's own log at `c:\logs\gnubox` on
the phone is the most direct way to tell which case you're in: if it shows
a Bluetooth connect attempt failing, the mRouter theory holds; if it's
empty, gnubox isn't being invoked at all and the phone is just trying to
place a real call to `2222`.

The firmware now automates that fallback (`main/bt_bootstrap.c`): right
after a fresh pairing completes, the ESP32 SDP-probes the phone, logs every
RFCOMM service it advertises (`[SDP] phone service: ch N name '...'`),
then connects once into the phone's Serial Port (0x1101) service — or, if
the phone doesn't expose one, its Dial-Up Networking (0x1103) service
instead (many Nokia S60 phones, e.g. the 7610, only expose modem/COM-port
access as DUN). It holds the link ~3 s and disconnects — the same
registration a PC serial connection performs. It runs once per
newly-paired address per boot, so to re-trigger it, remove and re-add the
pairing on the phone (or just reboot the ESP32 and pair again). After the
`bootstrap link closed — done` log line, retry the gnubox dial.

## Open investigation: S60 2nd Edition phones never dial (6600/7610 family)

Tested on a **Nokia 7610** (S60 2nd Edition FP1) running a gnubox build
targeted at the **6600** (also S60 2nd Ed) — a different Symbian generation
than the S60 1st Edition hardware this project is validated against. Result:
**still doesn't work**, even after fixing the two things we could fix:

- The mRouter bootstrap (`main/bt_bootstrap.c`) originally only searched the
  phone for a Serial Port (0x1101) service. The 7610 exposes no SPP at all —
  only Dial-Up Networking (0x1103) at channel 2. Bootstrap now falls back to
  DUN and **does** successfully connect into it, hold, and disconnect clean.
- gnubox's CommDB rewrite also works correctly on this phone: the `Bt`
  access point's dial-up number blanks out as expected after picking
  **Vetera Bridge** in `2box Direct`.

Despite both succeeding, the phone still never initiates an RFCOMM
connection to the ESP32 when the `Bt` access point is used afterward — zero
HCI activity on retry, every time. Confirmed via the phone side too: gnubox
only edits CommDB (the APN bearer pointer + dial-up number); it does not
drive the Bluetooth connection itself. Placing the actual call is entirely
up to a native OS component (what gnubox's own notes call "mRouter") that
apparently still isn't being triggered to dial out, even though every state
we can directly influence checks out.

Open question for future work: what does the native mRouter actually
require before it will dial a Bluetooth-bearer access point? Candidates
worth testing:

- ESP32 may need to itself expose a Dial-Up Networking **gateway** service
  (0x1103), not just LAP (0x1102) / SPP (0x1101) — S60 2nd Ed's Bluetooth
  modem stack may specifically SDP-search for a DUN gateway before dialing,
  since DUN's whole point is "use the remote device as your modem."
- The CommDB field/table that names the Bluetooth modem bearer may differ
  between S60 1st Ed and S60 2nd Ed (different comms architecture between
  Symbian 6.1 and 7.0s), so the one-shot bootstrap trick may be solving the
  wrong problem on this generation.
- The phone may need a real DUN session (as the *data terminal*, dialing
  through another device) to have completed at least once, caching modem
  parameters, before it will treat a new paired device as a valid modem —
  something stronger than the current one-shot "connect and disconnect"
  bootstrap.

S60 1st Edition (N-Gage Classic, 3650, 7650...) remains the fully validated,
working path — that's what's documented and supported above. S60 2nd
Edition (6600/7610 family) support is an open investigation, not working
end-to-end yet.

## Monitor log walkthrough

A successful session looks like:

```
[HCI] PIN request from <phone> — answering '0000'     (pairing, once)
incoming RFCOMM ch 3 from <phone> — accepting          (gnubox dial)
RFCOMM channel opened, cid 0x..., max frame ...
PPP server listening
pre-PPP RX (n bytes): 'CLIENT'                         (phone's dial script)
pre-PPP TX: replied to handshake                       (we say CLIENTSERVER)
first PPP flag seen — handing link off to PPP
PPP up, our 192.168.7.1 peer 192.168.7.2               (IPCP done)
[STATE] ... -> BRIDGE_ACTIVE                           (NAPT enabled, WiFi already up)
```

WiFi connects on its own as soon as the bridge boots (if networks are saved)
and stays connected across phone connects/disconnects — it does not wait
for a phone. If the phone dials before WiFi has finished connecting, the
log instead shows the scan/connect happening after PPP comes up:

```
PPP up, our 192.168.7.1 peer 192.168.7.2               (IPCP done)
[STATE] WAIT_BT -> WIFI_SCAN -> WIFI_CONN              (3 s after PPP up)
got IP, connected to '<ssid>'
[STATE] ... -> BRIDGE_ACTIVE                           (NAPT enabled)
```

Heartbeat lines (`[HB] State:... | BT:... | PPP:... | WiFi:...`) appear every
30 s with RSSI, heap, and TX-drop counters.

## Design notes

- `main/ppp_link.c` — the core: RFCOMM server ↔ lwIP PPPoS glue.
  RX goes straight to `pppos_input_tcpip()`; TX crosses from the lwIP tcpip
  task to the BTstack task through a stream buffer + can-send-now events.
  One PPP session per RFCOMM connection; a second connection is declined.
- `main/sdp_lap.c` — hand-built LAP SDP record (BTstack has no template for
  this deprecated profile).
- `main/wifi_multi.c` — NVS-stored network list + scan-and-pick connect.
- `main/gateway.c`, `web_ui.c`, `dns_fwd.c` — state machine, watchdogs, web
  UI and DNS derived from Satura Bridge's proven implementation.
- WiFi connects immediately at boot (if networks are saved) and stays up
  indefinitely, independent of the phone — it is never torn down when the
  phone disconnects. While a phone is mid-dial (BT connected, PPP not yet
  up), scan/connect attempts are deferred ~3 s so the scan's radio
  disruption (single shared radio) doesn't break LCP/IPCP; this covers both
  the post-PPP-up kick and any WiFi retry cycle that happens to be running
  when the phone dials in.
- `-Os` (`CONFIG_COMPILER_OPTIMIZATION_SIZE`) is load-bearing: with `-Og`
  the BT + WiFi + PPP combination overflows ESP32 IRAM.

Known limits: throughput is modest (BT Classic + WiFi coexistence on one
radio — fine for retro browsers); the phone's RFCOMM may negotiate small
frames (harmless — PPPoS is a byte stream); a CCP ConfReq from the phone
gets a spec-correct Protocol-Reject.

## Status

Builds clean against ESP-IDF v5.4.4 + BTstack v1.8.2. Validated end-to-end
against real S60v1 hardware (pairing → mRouter bootstrap → gnubox dial →
PPP → web UI → WiFi NAT). S60 2nd Edition (6600/7610 family) does not dial
yet — see "Open investigation" above. Bring-up reports for other phones
welcome.

## Credits & license

MIT — see [LICENSE](LICENSE).

Derived from [Satura Bridge](https://github.com/sigildeveloper/satura-bridge)
by **@sigdev** (MIT): the WiFi/NAT/DNS/watchdog architecture and the ESP32
BTstack port integration come from there. The RFCOMM/PPP path is new.
The gnubox technique is Mika Raento's (2004). BTstack (fetched into
`components/btstack/`) is distributed under its own license.
