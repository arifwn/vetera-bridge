# Vetera Bridge

**Bluetooth PPP → WiFi bridge for pre-PAN retro phones, on ESP32**

A sibling of [Satura Bridge](https://github.com/sigildeveloper/satura-bridge),
for the older generation it can't reach. Satura Bridge gives internet to retro
phones over **Bluetooth PAN** — but the oldest smartphones (Symbian 6.1 /
S60 1st Edition: Nokia N-Gage, 7650 and 3650) predate PAN entirely. Vetera
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
- **Bookmarks** (up to 24) shown as tappable links on the bridge's home page,
  so a phone never has to have a URL typed into it. Edit them from a PC on the
  same WiFi, using a real keyboard — every phone that dials in gets the list
- Web setup UI at **http://192.168.7.1** over the phone's own PPP link
  (plain HTML — renders in Opera for S60v1), with captive-DNS redirect:
  open any http:// page before WiFi is configured and you land in setup
- DNS forwarder with cache; legacy pairing with fixed PIN `0000`
- **Per-unit Bluetooth name**: each bridge advertises `Vetera Bridge <number>`
  derived from its own Bluetooth address, so several units can run at once
  without being confused in the phone's device list; rename any unit on `/name`
- Watchdogs: WiFi-stuck recovery, DNS-hang restart, low-heap reboot
- **Bluetooth security switch** on `/security`: makes the bridge authenticate
  and encrypt the link before a phone opens the serial channel. S60 2nd
  Edition phones with gnubox need it on; S60 1st Edition phones need it off
  (the default)
- **Debug logging switches** on `/debug`: HCI packet trace, BTstack's internal
  log and verbose bridge events, to the USB serial console. Kept across
  reboots, so a trace can cover pairing from power-on

## Hardware

Original **ESP32** (ESP32-D0WD family) — it must have Bluetooth Classic.
Later chips (S2/S3/C3/C6...) are BLE-only and will not work.
An external antenna helps noticeably: BT and WiFi share one radio.

## Flashing a prebuilt binary

You don't need to build from source — grab the binaries from the
[latest release](https://github.com/arifwn/vetera-bridge/releases/latest).

**Option A — web flasher, no software install:**

1. Open in Chrome or Edge: **<https://esp.huhn.me>** (Firefox does not
   support WebSerial)
2. Connect the ESP32 via USB
3. Click **Connect** → select the device's serial port
4. Click **Add File**, select the downloaded `vetera-bridge-vX.Y.Z-full-flash.bin`
5. Set address to **0x0**
6. Click **Program**, then **Reset** once it finishes

**Option B — esptool (Python):**

```sh
pip install esptool
python -m esptool --chip esp32 -p /dev/ttyUSB0 -b 460800 \
    --before default_reset --after hard_reset write_flash \
    0x0 vetera-bridge-vX.Y.Z-full-flash.bin
```

Adjust `/dev/ttyUSB0` to your serial port. Verify downloads against the
release's `SHA256SUMS.txt`.

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

**If you built a previous version in this tree, delete `sdkconfig` first:**

```sh
rm -f sdkconfig sdkconfig.old      # may need sudo: the Docker build writes it as root
```

`sdkconfig` is generated, gitignored, and **overrides `sdkconfig.defaults`** — so
an existing one silently keeps the old settings and a pull that changes a
default has no effect. That is how the v1.4.1 Bluetooth controller fix stays
invisible if you skip this step. For the same reason, Kconfig does not complain
about unknown symbols in `sdkconfig.defaults`: after changing anything there,
check the value actually landed in the generated `sdkconfig`.

## Cutting a release

`make-release.sh` builds the release binaries the way they are meant to be
built — from a scrubbed tree, so a release can never ship a binary carrying
settings from an old `sdkconfig`:

```sh
git tag -a v1.4.3 -m "v1.4.3" && git push origin v1.4.3
./make-release.sh v1.4.3                        # assets in build/release/
./make-release.sh v1.4.3 --publish notes.md     # ...and create the release
```

It refuses to run on a dirty tree, or when the tag exists but does not point at
`HEAD`, and checks that the generated `sdkconfig` really came out in BR/EDR-only
mode before it will hand you anything. Note that builds are not bit-for-bit
reproducible: `CONFIG_APP_COMPILE_TIME_DATE` is on, so rebuilding the same
commit gives a different app-image hash.

## Phone setup (S60 1st Edition. Tested on the N-Gage Classic, Nokia 3650 and Nokia 7650)

Leave the bridge's Bluetooth security switch (`/security`) **off**, which is
the default. With it on, an N-Gage QD cannot connect.

1. **Pair**: phone → Connectivity → Bluetooth → Paired devices → New paired
   device → select **Vetera Bridge &lt;number&gt;** → PIN `0000`.
   Each bridge names itself after its own Bluetooth address, so two units
   powered on at once are told apart in the device list. You can give a unit a
   friendlier name later on the `/name` page (see below).
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
   pick your bridge.
4. Open the browser (Opera recommended — it speaks plain HTTP, unlike the
   built-in WAP `Services` browser) with access point `Bt` and load
   `http://192.168.7.1`. This works before any WiFi is configured.
   Add your WiFi network(s) on the `/wifi` page, and rename the unit on the
   `/name` page if you run more than one bridge.
   An official Opera Mobile build for S60 1st Edition (N-Gage etc.) is
   still available from Opera's FTP archive:
   <https://ftp.opera.com/pub/opera/series60/1.x/620/apac/opera_s60_620_asia_61_10.sis>
5. Load a plain-HTTP page from the internet, e.g. `http://frogfind.com`.
   HTTPS-only sites will fail — that's the phone's ancient TLS, not the link.

### Bookmarks

Typing an address on a keypad — especially the 3650's rotary one — gets old
fast, and doing it once per phone is worse. Instead, keep the list on the
bridge: bookmarks show up as tappable links right at the top of
`http://192.168.7.1`, so browsing is open-the-page-and-select.

Manage them on the `/bm` page. The web UI is reachable from any computer on
the same WiFi as the bridge — use the **WiFi IP** shown on the status page,
e.g. `http://192.168.1.42/bm` — so the typing happens on a real keyboard, once,
and every phone that dials in sees the same list. The `/bm` page works from the
phone too; it's the same page either way.

- Up to **24** bookmarks. The limit is what a 176×208 screen can usefully
  render, not flash: bookmarks share the ESP32's small NVS partition with the
  WiFi list and the Bluetooth pairing keys, and staying well clear of it keeps
  those safe. The boot log prints the actual NVS headroom
  (`I (…) bookmarks: … NVS … free`) if you want to raise `BM_MAX` in
  `main/gateway.h`.
- `http://` is added for you if you leave it off. Addresses must be `http://`
  or `https://`, and at most 159 characters.
- Prefer plain `http://` targets — HTTPS won't load on an S60v1 phone.

Bookmarks are an **Opera** feature. The built-in `Services` browser can't see
them: it speaks WAP/WSP through a WAP gateway (see below), and a public gateway
out on the internet has no route to `192.168.7.1` sitting behind your phone's
PPP link.

#### A starter list

Finding sites an S60v1 phone can still open is most of the work — the modern
web is HTTPS-only, and its TLS is far too old to negotiate with anything
current. Every address below was checked to return **200 over plain HTTP with
no HTTPS redirect**, which is the property that actually matters. Sizes are the
homepage payload; anything much past ~50 KB starts to strain Opera on a 3650.

**Search — the highest-value entries, since one bookmark reaches everything else**

| Address | Size | |
| --- | --- | --- |
| `http://wiby.me` | 1.5 KB | Search engine built for old, lightweight pages |
| `http://wiby.me/surprise/` | 440 B | A random classic-web page, one tap. Made for a keypad |
| `http://frogfind.com` | 2 KB | Search, plus a reader that strips any page to bare HTML |

**WAP portals that are still running**

| Address | Size | |
| --- | --- | --- |
| `http://waptrick.com` | 5 KB | XHTML-MP. Themes, wallpapers, ringtones, J2ME games |
| `http://zamob.com` | 5 KB | Same engine, different branding |
| `http://dedomil.net` | | J2ME game archive, organised by handset |

**Gopher over HTTP** — menus are plain text link lists, close to ideal on 176×208

| Address | Size | |
| --- | --- | --- |
| `http://gopher.floodgap.com/gopher/` | 10 KB | Gateway into all of gopherspace |
| `http://www.floodgap.com` | 16 KB | Cameron Kaiser's retro-computing site |

**Text and retro archives**

| Address | Size | |
| --- | --- | --- |
| `http://68k.news` | 4 KB | Current news, rendered for vintage browsers |
| `http://www.textfiles.com/directory.html` | 5.8 KB | Better entry point than the root |
| `http://info.cern.ch` | 646 B | The first website; every onward link is plain HTTP |
| `http://oldcomputers.net` | 1.5 KB | |
| `http://retrocomputing.net` | 6 KB | |
| `http://sunsite.icm.edu.pl` | 3 KB | Long-running FTP/HTTP mirror |
| `http://kolibrios.org/en` | 4.8 KB | An OS written in assembly |

**Useful for setup**

| Address | Size | |
| --- | --- | --- |
| `http://nbpfan.bs0dd.net` | 6.5 KB | The public WAP-gateway list — needed for the built-in browser, below |
| `http://neverssl.com` | 4 KB | Guaranteed plain HTTP: a clean "is the bridge working" test |

Two things worth knowing. Sites drift to HTTPS constantly, so a bookmark that
worked last year may simply stop — if a page dies, that is usually why, not the
bridge. And when a site you want is too heavy, bookmark it through FrogFind's
reader instead: `http://frogfind.com/read.php?a=http://example.com/page` returns
the same content as bare HTML, often several times smaller.

### Using the phone's built-in browser instead of Opera

If you'd rather not install Opera, the built-in WAP `Services` browser can
be used instead — but unlike Opera it doesn't speak plain HTTP directly, it
only speaks WAP/WSP and needs a **WAP gateway** configured on the access
point to translate that into HTTP on the ESP32/internet side. Set the
access point's Gateway IP to a public WAP gateway's address instead of
leaving it blank. A list of currently available public WAP gateways is
maintained at
<http://nbpfan.bs0dd.net/index.php?lang=eng&page=wap/main>.

**Caution:** a public WAP gateway is an open proxy run by a stranger on the
internet — every request and response, including anything you type into a
form, passes through their server in the clear. Only use these for
casual/retro browsing, and never enter passwords, personal details, or any
other sensitive information through a random public gateway.

If the phone connects to nothing after gnubox, it may not have stored the
picked device: some gnubox builds never write the default BT comm port.
Symptom: the `Bt` access point's Dial-up number stays `2222` instead of
being blanked, and the ESP32's serial log shows zero HCI activity at all
(no connection request, nothing) when you try to browse — the phone isn't
even attempting a Bluetooth connection.

Fix that has worked in practice: in gnubox, switch `Options → 2box Direct`
to `Infrared`, then switch it back to `Bluetooth → your bridge`. This
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

## Phone setup (S60 2nd Edition FP1. Tested on the Nokia 7610)

S60 2nd Edition needs one thing S60 1st Edition must not have: **Bluetooth
security on the bridge**. gnubox always puts a security requirement on the
phone's Bluetooth serial port, and the phone then refuses an unencrypted link,
but it never starts the authentication or encryption itself. It expects the
other end to. Without it, the phone opens the bridge's serial channel, sends
nothing, and hangs up after about 6 s ("network problem" in Opera). This is
what the old Windows tutorials meant by ticking "Secure Connections" (Widcomm)
or "Enable Data Encryption" (BlueSoleil). The full investigation is in
[S60V2-FINDINGS.md](S60V2-FINDINGS.md).

1. **Turn on security**: open `/security` on the bridge, tick *Require
   authentication and encryption* and save. The bridge reboots to apply it.
   Do this from a PC on the same WiFi (at the bridge's WiFi IP) or from an
   S60 1st Edition phone dialled in. **Known limitation:** a brand-new bridge
   has no WiFi yet, and the 7610 cannot reach the web UI until security is
   on, so a 7610 alone cannot set up a new unit. Configure WiFi and security
   once with an S60 1st Edition phone first.
2. **Pair**: phone → Connectivity → Bluetooth → pair with **Vetera Bridge
   &lt;number&gt;** → PIN `0000`.
3. **Access point** named exactly `Bt`, bearer **Data call** (not GPRS),
   dial-up number something undiallable (e.g. `321`; gnubox blanks it),
   Authentication `Normal`. The CLIENT/SERVER login script from the gnubox
   tutorials is fine to set: the bridge answers it if the phone sends it.
4. Install the matching gnubox: **-xan-'s v1.1 beta**, `gnubox_7610.sis`
   (shared with the 6670; the 3230, 6260 and 6620 have their own builds). Not
   Mika Raento's 6600 build, which targets a different comms architecture.
   **Back up the phone first** and create `c:\logs\gnubox`. Then run gnubox →
   `Options → Install → Create records`, exit gnubox with `Options → Exit` and
   relaunch it.
5. gnubox → `2box → Direct over BT/Serial` → pick the bridge. At "Do you want
   to require encryption?" press the **left** soft key.
6. **Check the link**: gnubox → `Options → Debug → Test comm channel`, then
   open `c:\nokia\ctest.txt`. Healthy is `123456Error = 0` and
   `78 Error = -3`. `Error = -6005` means the bridge's security switch is
   off (or not applied yet).
7. Open Opera with access point `Bt` and load `http://192.168.7.1`.

Only the 7610 has been tested. The other FP1 phones gnubox v1.1 supports
(3230, 6260, 6620, 6670) should behave the same with their own gnubox build.

The bridge also advertises a Dial-Up Networking (0x1103) record next to LAP
and Serial Port, on the Serial Port channel, and answers `AT`/`ATD` like a
modem, for any phone setup that dials over DUN.

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

### Debug logging

When the normal log is not enough, open `/debug` (from the phone at
`http://192.168.7.1/debug`, or from a PC at the bridge's WiFi IP) and tick:

- **HCI packet trace**: every HCI packet as hex. Shows SDP queries, RFCOMM
  setup and pairing, the layer where a phone that "won't dial" usually fails.
- **BTstack internal log**: BTstack's own `LOG --` lines (L2CAP/RFCOMM/SDP).
- **Verbose bridge events**: unhandled HCI event codes, and the link security
  level when a phone opens the serial channel.

Changes apply at once and are stored in NVS, so they survive a reboot and the
trace can start at power-on. Output goes to the USB serial console only. The
packet trace prints from inside the Bluetooth run loop over a 115200-baud
UART, so it slows the phone's connection a lot: turn it off for normal use.

## Design notes

- `main/ppp_link.c` — the core: RFCOMM server ↔ lwIP PPPoS glue.
  RX goes straight to `pppos_input_tcpip()`; TX crosses from the lwIP tcpip
  task to the BTstack task through a stream buffer + can-send-now events.
  One PPP session per RFCOMM connection; a second connection is declined.
- `main/sdp_lap.c` — hand-built LAP SDP record (BTstack has no template for
  this deprecated profile).
- `main/wifi_multi.c` — NVS-stored network list + scan-and-pick connect.
- `main/bt_name.c` — the advertised device name. Composed once the controller
  reports its BD_ADDR (`BTSTACK_EVENT_STATE` / `HCI_STATE_WORKING`), since the
  default suffix is derived from that address; a custom name from `/name` is
  kept in NVS and wins. Renames re-run `gap_set_local_name` on the BTstack task.
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
PPP → web UI → WiFi NAT), and on the S60 2nd Edition FP1 Nokia 7610 with
the `/security` switch on. Bring-up reports for other phones welcome.

## Credits & license

MIT — see [LICENSE](LICENSE).

Derived from [Satura Bridge](https://github.com/sigildeveloper/satura-bridge)
by **@sigdev** (MIT): the WiFi/NAT/DNS/watchdog architecture and the ESP32
BTstack port integration come from there. The RFCOMM/PPP path is new.
The gnubox technique is Mika Raento's (2004). BTstack (fetched into
`components/btstack/`) is distributed under its own license.
