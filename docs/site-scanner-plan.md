# Site connectivity scanner: plan

As of 2026-11-04, 9:40 PM EST. This is a plan only: nothing here is built until Joe
approves the requirements marked `[proposed]`.

## Goal

Take one board to a candidate sensor site, scan every link the sensor could use, and
come away with a report that says, for each connection, whether it is usable there, how
well it works, and what it would cost per year. The base is the Heltec WiFi LoRa 32 V3
(ESP32-S3 with WiFi and BLE, an SX1262 LoRa radio and a 0.96" OLED). It already runs the
LoRaWAN survey (`commissioning/lora-survey` in tdisplay-s3-amoled), which becomes one
test in the scanner.

## What the Heltec can test, and what it needs

| Connection | Heltec alone | Add-on needed | Add-on cost |
|---|---|---|---|
| **WiFi** | Yes: scan and, with credentials, a real connect test | none | $0 |
| **LoRaWAN** (TTN, Helium) | Yes: join and LinkCheck at SF7 – SF10 (the survey program) | none | $0 |
| **LoRa point-to-point** | Half: it needs a partner radio at the base | a second Heltec V3 as a beacon where the internet is | ~$20 – 25 *(approx.)* |
| **Cellular** | No: the Heltec has no modem | the Notecard Cell+WiFi and Notecarrier-X you own, on I2C (Qwiic cable) | $0 (owned) |
| **Satellite, Skylo** | No | Notecard for Skylo (LTE-M/NB-IoT + Skylo), plugs into the same Notecarrier-X | $89 |
| **Satellite, Iridium** | No | Starnote for Iridium kit (Notecard, Notecarrier XI, antenna) | $409 |
| **Sky view** (for any satellite) | Not measurable by radio alone | a phone satellite-finder or compass app, noted in the report | $0 |

BLE is left out: it is only for setup at arm's length, not a site link.

So the base kit (Heltec + the Notecard you own) covers WiFi, LoRaWAN and cellular.
Adding a Notecard for Skylo ($89) covers cellular and Skylo with one card. Iridium is
worth testing only if a site fails everything else.

## What each test measures, and the pass marks

Each connection gets one verdict per spot: **Good**, **Marginal**, **Not usable**, or
**Not tested**.

| Connection | Test | Measures | Good | Marginal | Not usable |
|---|---|---|---|---|---|
| WiFi | Scan 30 s, all channels; with credentials, connect, DHCP, DNS, one HTTPS request to the ThingsBoard or ntfy URL | per network: SSID, channel, RSSI, security; connect time, HTTPS round trip | RSSI ≥ −67 dBm and the HTTPS request succeeds | −67 to −75 dBm, or it connects only some of the time | below −80 dBm, or no connect |
| LoRaWAN | 3 – 5 ladder rounds (SF7 → SF10, LinkCheck) | margin, gateways, lowest SF that answers, downlink RSSI/SNR | SF7 answers with margin ≥ 10 dB | answers at SF7 with < 10 dB, or only at SF8 – SF10 | no join, or no answer at any SF |
| LoRa P2P | Ping-pong with the beacon at SF7 – SF12, 10 pings each | packets received both ways, RSSI, SNR | ≥ 9 of 10 at SF9 or lower, SNR ≥ 0 dB | ≥ 9 of 10 only at SF10 – SF12 | < 9 of 10 at SF12 |
| Cellular | Notecard `card.wireless`, then one sync to Notehub (a few kB) | RAT, band, carrier, RSRP, RSRQ, SINR, bars, time to connect | RSRP ≥ −102 dBm, SINR ≥ 5 dB, sync < 60 s | RSRP −102 to −111 dBm, or a slow sync | RSRP below −111 dBm, or no sync |
| Satellite, Skylo | One compact 50-byte NTN note with `card.transport` `ntn` (costs about $0.04) | time until Notehub receives it; sky view noted by hand | arrives within 10 min | arrives within 60 min | not received within 60 min |
| Satellite, Iridium | Same with Starnote for Iridium (10-byte note) | as above | arrives within 10 min | within 60 min | not received |

The RSRP bands are Blues' published ones (excellent above −84, good to −102, fair to
−111, poor below). The LoRaWAN bands match the survey's guide. The WiFi bands are the
usual −67 dBm design point for reliable links. All of them are **[proposed]**, for Joe to
change.

Each reading is taken where the sensor would be mounted. A note on the report records
whether the board was inside the planned enclosure, because an enclosure costs a few dB.

## The report

The scanner opens its own WiFi access point (`site-scan-XXXX`). A phone joins it and
opens a page with:

1. **Spot details**, typed in: site name, spot name, latitude and longitude (pasted from
   the phone's map), mounting height, enclosure yes or no, sky view toward the south
   (clear, partly blocked, blocked).
2. **Run tests**: a button for each test and one for "all available". It shows progress.
   The OLED shows the same, so a run works without a phone.
3. **Results table**, one row per connection: verdict, key numbers, and the yearly data
   cost from the feasibility analysis for the chosen send pattern.
4. **Recommendation**: the cheapest connection that is Good, with Marginal ones listed as
   fallbacks. The order of preference is WiFi (free), cellular, LoRaWAN (if a gateway is
   in reach), LoRa P2P, Skylo, Iridium.
5. **Download** the spot report as Markdown (to paste into notes) and the raw readings as
   CSV. Reports are kept in flash, so several spots can be scanned and downloaded later.

The phone's browser can't hand over its GPS position over plain HTTP, which is why the
coordinates are typed or pasted in. The Notecard's GPS can fill them in when a Notecard
is attached (one fix, a minute or two outdoors).

Example of a report row:

```text
| Cellular | Good | LTE Cat-1 bis, band 12, AT&T, RSRP -97 dBm, SINR 9 dB, sync 31 s | 500 MB/10 yr included |
```

## Build plan

The firmware lives in this repo, built with ESP-IDF v6.0.2 like the other projects, and
reuses RadioLib and the shared components where they fit (`notecard`, the OLED screen).

| Phase | Adds | Hardware | Effort |
|---|---|---|---|
| 1 | The scanner frame: access point, report page, spot details, flash storage, Markdown and CSV download; WiFi scan and connect test; the LoRaWAN survey moved in as a test | Heltec only | largest phase |
| 2 | Cellular test through the Notecard on I2C, and the GPS fix | + Notecarrier-X and Notecard (owned) | small: the `notecard` component already exists |
| 3 | Skylo test with a compact NTN template | + Notecard for Skylo ($89) | small |
| 4 | LoRa P2P beacon firmware and ping-pong test | + second Heltec V3 | medium |
| 5 | Iridium test, only if a site needs it | + Starnote for Iridium kit ($409) | small, same as 3 |

Wiring for phase 2: the Notecarrier-X joins the Heltec by a Qwiic cable on its own I2C
bus, on two free header GPIOs picked from the V3 pinout, with the cable's 3.3 V wire
left unconnected as in the T-Display build. The Notecarrier brings its own LiPo for the
modem's 2 A peaks.

## Requirements to approve

- **SCN-1** [proposed] The scanner shall test, at one spot, each connection whose
  hardware is attached: WiFi, LoRaWAN, LoRa P2P, cellular, Skylo, Iridium.
- **SCN-2** [proposed] Each test shall give a verdict (Good, Marginal, Not usable, Not
  tested) by the pass marks above, with the measured values.
- **SCN-3** [proposed] The scanner shall serve a report page from its own access point and
  show progress and verdicts on the OLED.
- **SCN-4** [proposed] A spot report shall carry the spot details typed on the page and be
  downloadable as Markdown and CSV, kept in flash for at least 20 spots.
- **SCN-5** [proposed] The report shall recommend the cheapest Good connection, using the
  yearly costs of the feasibility analysis.
- **SCN-6** [proposed] Keys and credentials (LoRaWAN keys, WiFi password, Notehub product)
  shall never be in git; they are entered on the page or in a local file.
- **SCN-7** [proposed] A satellite test shall send at most one note per run, and the page
  shall show its cost before it runs.

## Open questions for Joe

1. Should the Notecard for Skylo ($89) be bought now, so phase 3 can follow phase 2?
2. Is a second Heltec for the P2P test worth it, or is P2P out of scope for now?
3. Which send pattern should the cost column assume: a daily summary, or every reading?
