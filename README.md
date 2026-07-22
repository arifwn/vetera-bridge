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
5. Load a plain-HTTP page from the internet, e.g. `http://frogfind.com`.
   HTTPS-only sites will fail — that's the phone's ancient TLS, not the link.

If the phone connects to nothing after gnubox, it may not have stored the
picked device: some gnubox builds never write the default BT comm port.
Fix: trigger the phone's mRouter to connect out once (connect to the phone's
serial port from another device) so it stores the bridge as its default
Bluetooth serial device.

## Monitor log walkthrough

A successful session looks like:

```
[HCI] PIN request from <phone> — answering '0000'     (pairing, once)
incoming RFCOMM ch 3 from <phone> — accepting          (gnubox dial)
RFCOMM channel opened, cid 0x..., max frame ...
PPP server listening
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
- WiFi connect is deliberately deferred until ~3 s after PPP is up so the
  scan's radio disruption (single shared radio) doesn't break LCP/IPCP.
- `-Os` (`CONFIG_COMPILER_OPTIMIZATION_SIZE`) is load-bearing: with `-Og`
  the BT + WiFi + PPP combination overflows ESP32 IRAM.

Known limits: throughput is modest (BT Classic + WiFi coexistence on one
radio — fine for retro browsers); the phone's RFCOMM may negotiate small
frames (harmless — PPPoS is a byte stream); a CCP ConfReq from the phone
gets a spec-correct Protocol-Reject.

## Status

Builds clean against ESP-IDF v5.4.4 + BTstack v1.8.2. Not yet validated
against real S60v1 hardware — bring-up reports welcome.

## Credits & license

MIT — see [LICENSE](LICENSE).

Derived from [Satura Bridge](https://github.com/sigildeveloper/satura-bridge)
by **@sigdev** (MIT): the WiFi/NAT/DNS/watchdog architecture and the ESP32
BTstack port integration come from there. The RFCOMM/PPP path is new.
The gnubox technique is Mika Raento's (2004). BTstack (fetched into
`components/btstack/`) is distributed under its own license.
