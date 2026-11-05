# Site connectivity scanner: plan

As of 2026-11-04, 9:45 PM EST. This is a plan only: nothing here is built until Joe
approves the requirements marked `[proposed]`.

## Goal

Take a board to a candidate field location, test each link a sensor there could use, and
come away with a report that says, for each connection, whether it works there and how
well. The question is **feasibility at that location**. Cost is out of scope for now.

## Scope

| Connection | Now | Hardware |
|---|---|---|
| **WiFi** | Yes | the Heltec WiFi LoRa 32 V3 alone |
| **LoRaWAN** (TTN, Helium) | Yes | the Heltec alone (the survey in `firmware/survey` moves in as one test) |
| **LoRa point-to-point** | Yes | two Heltec boards: a **field unit** at the location and a **base beacon** where the data would land |
| Cellular, satellite | Later | Notecard on I2C, Notecard for Skylo, Starnote for Iridium (see the feasibility analysis) |

**Boards.** The Heltec V3 on hand is the first unit. The GPS Heltec boards on order become
the field units when they arrive: their GPS stamps each spot's position, so nothing has
to be typed in. The V3 then serves as the base beacon. Until then, the V3 can test WiFi
and LoRaWAN alone. Point-to-point needs two boards, so it waits for the first GPS board.
The firmware keeps the pins for each board in one table, so the same build targets both.
*Check which GPS model arrives:* a board with a TFT instead of the OLED (such as the
Wireless Tracker) needs its own screen driver.

## What each test measures, and the pass marks

Each connection gets one verdict per spot: **Good**, **Marginal**, **Not usable**, or
**Not tested**, with the measured values.

| Connection | Test | Measures | Good | Marginal | Not usable |
|---|---|---|---|---|---|
| WiFi | Scan 30 s on all channels. With credentials, also connect, get DHCP and DNS, and make one HTTPS request to a URL you give (for example the ThingsBoard server) | per network: SSID, channel, RSSI, security; connect time, HTTPS round trip | RSSI ≥ −67 dBm and the HTTPS request succeeds | −67 to −75 dBm, or it connects only some of the time | below −80 dBm, or no connect |
| LoRaWAN | 3 – 5 ladder rounds (SF7 → SF10, LinkCheck), run once on TTN and once on Helium, each a verdict of its own | margin, gateways, lowest SF that answers, downlink RSSI and SNR | SF7 answers with margin ≥ 10 dB | answers at SF7 with < 10 dB, or only at SF8 – SF10 | no join, or no answer at any SF |
| LoRa P2P | Ping-pong with the base beacon, 10 pings at each step: SF7 – SF10 at 125 kHz, then SF11 – SF12 at 500 kHz | packets that arrive each way, RSSI, SNR at both ends, the fastest SF with ≥ 9 of 10 | ≥ 9 of 10 at SF9/125 kHz or faster, and SNR at least 5 dB above that SF's floor | ≥ 9 of 10 only at SF10/125 kHz or SF11 – SF12/500 kHz | < 9 of 10 at every step |

The WiFi bands are the usual −67 dBm design point for a reliable link. The LoRaWAN bands
match the survey's guide. The P2P floor is the SX1262's demodulation limit: about
−7.5 dB SNR at SF7, falling 2.5 dB per SF to −20 dB at SF12. All of them are
**[proposed]**, for Joe to change.

Readings are taken where the sensor would be mounted, at its height. The report notes
whether the board was inside the planned enclosure, because an enclosure costs a few dB.

## TTN and Helium

The two networks have different gateways, so a spot can have one without the other, and
rural coverage often differs most. One device can't be registered on both with the same
DevEUI, so the scanner holds two registrations (two DevEUIs and keys) and joins each in
turn. Helium is LoRaWAN 1.0.x only, so its registration has no NwkKey. Its messages are
paid in data credits ($0.00001 per 24 bytes), which is nothing for a survey, but the
account needs a small prepaid balance. One thing to check on the first run is whether
the Helium network server answers LinkCheckReq. If it doesn't, the test falls back to
confirmed uplinks, and the ACK shows the uplink was heard; the margin and gateway count
then come only from the Helium console.

## Point-to-point test in detail

- **The base beacon** sits where the real receiver would be: a building with power and
  internet, its antenna placed as a gateway's would be. It listens all the time, answers
  each ping, and logs every exchange to its serial port.
- **The field unit** sends a ping carrying a sequence number, its SF and its transmit
  power. The beacon answers with the RSSI and SNR at which it heard the ping, so one
  exchange measures both directions. A missing answer counts as lost, and the field unit
  moves on after a timeout.
- **Radio settings, US 902 – 928 MHz**, the same rules LoRaWAN's US915 plan follows.
  At 125 kHz, FCC part 15.247 allows LoRa as a hybrid system: it hops over the 8
  channels from 902.3 to 903.7 MHz (200 kHz apart, sub-band 1, which keeps it off the sub-band 2 that TTN and Helium gateways listen on), and each packet
  stays under 400 ms on air. That keeps 125 kHz to **SF7 – SF10**, with a ping of at
  most 11 bytes at SF10. **SF11 and SF12 run at 500 kHz** on 903.0 MHz, which as a
  wideband (DTS) signal needs no hopping and costs about 6 dB of sensitivity. Both ends
  pick the channel from the sequence number, so they always agree. It sends at 20 dBm.
  **[proposed]**
- **Pairing.** Both units share a 16-byte network key and a network ID set at build
  time. Pings are authenticated (AES-CMAC over the payload) so a stray packet doesn't
  count as an answer. The key stays out of git.
- **What the result means.** The fastest step that passes says how often a sensor at the
  spot could send and how much battery each message costs. SF7 – SF9 is comfortable.
  SF10 and the 500 kHz steps work, but they allow only short messages, each with more
  airtime.

## The report

The phone talks to the field unit over **Bluetooth**, from a web page (`web/index.html`,
served over HTTPS from GitHub Pages). It works in Chrome on Android, and in the Bluefy
browser on an iPhone, since Safari has no Web Bluetooth. The phone keeps its own
internet, and the page can use the phone's GPS. The link is paired once with a 6-digit
code shown on the board's screen, the same way the T-Display's Bluetooth setup works
(the shared `ble_setup` component). The page has:

1. **Spot details**: site name, spot name, mounting height, enclosure yes or no, notes.
   Position comes from the phone's GPS with one button (or from the GPS board later).
2. **Run tests**: a button for each test and one for "all". It shows progress. The OLED
   shows the same, so a run also works without a phone, started with the PRG key.
3. **Results table**: one row per connection, with the verdict and its key numbers.
4. **Keys and WiFi**: the LoRaWAN keys for TTN and Helium and the WiFi network to try,
   stored on the board. They are written over the paired link and never read back.
5. **Download**: the spot report as Markdown (to paste into notes) and every spot's
   readings as CSV. Reports are kept in flash, so several spots can be scanned and
   downloaded later.

Example rows:

```text
| WiFi     | Good       | "FarmShop" ch 6, WPA2, -61 dBm; HTTPS to tb.example.org 420 ms |
| LoRaWAN  | Not usable | no join after 4 tries (SF10)                                     |
| LoRa P2P | Marginal   | fastest passing SF10/125: 10/10 out, 9/10 back, SNR -11 / -12 dB |
```

## Build plan

The firmware lives in this repo, built with ESP-IDF v6.0.2 like the other projects, with
RadioLib for the SX1262 and the probes' SSD1306 screen.

| Phase | Adds | Hardware |
|---|---|---|
| 1 | The scanner frame: the Bluetooth page, spot details, flash storage, Markdown and CSV download; the WiFi test; the LoRaWAN survey moved in as a test | Heltec V3 |
| 2 | The base beacon firmware and the P2P test. Direction: the bearing and distance to a beacon or gateway whose position is known, and a "turn slowly" test that plots signal strength against the phone's compass heading with a small directional 915 MHz antenna (a patch or a short Yagi, about 6 – 9 dBi, beam about 60°; at 20 dBm out that stays under the 36 dBm EIRP limit of part 15.247). A mount that holds the board on the back of the phone, powered from the phone's USB-C port (about 150 mA at most), so the antenna turns with the compass; the antenna stands off the phone on a short pigtail, because the phone and the hand detune it | V3 + first GPS board |
| 3 | GPS position from the board; the GPS board's pins (and its screen, if it differs) | GPS board |
| later | Cellular and satellite tests | Notecard parts |

Phase 1 can be built and field-tested now. Phase 2 can be written now and tested when the
first GPS board arrives.

## Requirements to approve

- **SCN-1** [proposed] The scanner shall test WiFi, LoRaWAN and LoRa point-to-point at one
  spot, each test started on its own or all together.
- **SCN-1a** [proposed] The LoRaWAN test shall run on The Things Network and on Helium,
  each with its own device registration and keys, and report a verdict for each.
- **SCN-2** [proposed] Each test shall give a verdict (Good, Marginal, Not usable, Not
  tested) by the pass marks above, with the measured values.
- **SCN-3** [proposed] The field unit shall be run and read from a Web Bluetooth page
  (Chrome on Android, Bluefy on iPhone), and show progress and verdicts on its screen.
- **SCN-4** [proposed] A spot report shall carry the spot details and position, and be
  downloadable as Markdown and CSV. At least 20 spots are kept in flash.
- **SCN-5** [proposed] The P2P test shall run between a field unit and a base beacon, hop
  over 8 channels at 125 kHz (SF7 – SF10) and use 500 kHz for SF11 – SF12, and measure
  both directions in each exchange.
- **SCN-6** [proposed] Keys and credentials (LoRaWAN keys, the P2P key, the WiFi password)
  shall never be in git. They are entered on the page or in a local file.
- **SCN-7** [proposed] The same firmware shall run on the Heltec V3 and on the GPS board,
  with each board's pins in one table.
