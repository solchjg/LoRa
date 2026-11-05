# Site scanner: setup and use

The scanner (`firmware/scanner`) is phase 1 of [the plan](site-scanner-plan.md). It runs
on a **Heltec WiFi LoRa 32 V3**. At each spot it tests WiFi, The Things Network and
Helium, gives each a verdict (Good, Marginal, Not usable, Not tested) by the plan's pass
marks, and keeps the last 20 spots in flash. A phone runs it over Bluetooth from the
page in `web/index.html`.

## Hardware

- The Heltec V3 in its 863 – 928 MHz variant, with its 915 MHz antenna. **Screw on the
  antenna before powering the board**: transmitting without one can damage the SX1262.
- A USB-C cable, and for field use a power bank (or the phone itself, from its USB-C
  port).

## 1. Register the LoRaWAN devices

Each network needs its own registration, with its own DevEUI and keys. One device
can't be on both with the same DevEUI.

- **The Things Network**: in the [TTN console](https://console.cloud.thethings.network)
  (cluster *nam1*), register an end device manually: frequency plan **United States
  902-928 MHz, FSB 2**, LoRaWAN **1.1.0**, regional parameters **RP002 1.0.4**, JoinEUI
  all zeros, generated DevEUI, AppKey and NwkKey. Under *Join settings*, tick **Resets
  join nonces**.
- **Helium**: in a Helium console (for example a ChirpStack-based one), add a device
  as **LoRaWAN 1.0.x, US915**. It has a DevEUI, JoinEUI and AppKey only. The account
  needs a small prepaid data-credit balance.

You type these keys into the page (step 4). They are kept on the board, never read
back, and never in git.

## 2. Build and flash

With IDF v6.0.2 exported, or in the `espressif/idf:v6.0.2` container:

```bash
git submodule update --init          # the shared components (ble_setup, text)
cd firmware/scanner
idf.py -B build -DSDKCONFIG=$PWD/build/sdkconfig set-target esp32s3
idf.py -B build -DSDKCONFIG=$PWD/build/sdkconfig build
idf.py -B build -DSDKCONFIG=$PWD/build/sdkconfig -p /dev/ttyUSB0 flash monitor
```

The V3's port is a CP2102 USB-UART bridge (`/dev/ttyUSB0`, or `COMx` on Windows).
RadioLib 7.8.1 is fetched from GitHub on the first build. Each push also builds the
firmware in CI, and the run keeps the images as an artifact.

Settings under *Site scanner* in `menuconfig`: US915 sub-band (2), transmit power
(20 dBm), ladder rounds per network (3), join tries (3), WiFi connect attempts (3).

## 3. Open the page and pair

The page is published on GitHub Pages from `web/` (Settings → Pages → Source **GitHub
Actions**, once). Open it in **Chrome on Android** or in **Bluefy on iPhone**; Safari has
no Web Bluetooth.

Press **Connect to scanner** and pick the board (`SCAN-xxxxxx`). The first time, the
phone asks for a code: type the 6 digits the board shows. The phone stays paired after
that.

## 4. Keys and WiFi

Under **Keys and WiFi**, type each network's DevEUI, JoinEUI and AppKey (and TTN's
NwkKey), and the WiFi network to try with a URL to fetch, for example the ThingsBoard
server. Leave a key box empty to keep the stored key; type `clear` to remove it. A
network without keys is reported as Not tested.

## 5. At each spot

1. Put the board where the sensor would be mounted, at its height, inside the planned
   enclosure if there is one.
2. Fill in **Spot**: site, spot name, height, enclosure, notes. **Use phone position**
   fills in the latitude and longitude. Press **New spot** for a new place, or **Save**
   to change the current one.
3. Press **All** under **Run tests** (or one test). The page and the board's screen show
   each step; a full run takes a few minutes. Without the phone, a **short press** of
   PRG runs all tests, and a **long press** (1 s) starts a new spot.
4. **Results** shows the verdicts; **Show the full report** shows the measured values.

## 6. Download

The **Spots** list holds every spot on the board, newest first. **.md** downloads one
spot's report as Markdown; **Download all as CSV** downloads every spot's readings, one
line per spot and connection (and one per network heard, and per spreading factor).
**open** makes an earlier spot the current one, to retest it.

## What the tests do

- **WiFi**: a scan of all channels keeps the 8 strongest networks. With a network set, the
  board connects a few times and times each connect, then fetches the URL once over
  HTTPS. Good is −67 dBm or better with a successful fetch.
- **LoRaWAN**: the board joins (US915 sub-band 2) and runs ladder rounds: SF7 first,
  then SF8, SF9 and SF10, stopping at the first answer. Each uplink carries a
  LinkCheckReq, so the answer gives the margin and the number of gateways. Helium gets
  confirmed uplinks too, in case its network server doesn't answer LinkCheckReq. Good is
  an answer at SF7 with a margin of 10 dB or more. Ladder rounds keep TTN's fair-use
  limits (10 downlinks a day).
