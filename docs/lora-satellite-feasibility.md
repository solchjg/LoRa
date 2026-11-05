# LoRa and satellite links for remote sites: feasibility and cost

As of 2026-11-04, 7:30 AM EST. Planning note only: no FSD rule covers any of this yet,
so nothing here is built until `/define` turns a choice into approved requirements.
Prices are from the vendors' pages listed under Sources on this date, except where a
row says *approx.*, which means I could not confirm it today and it needs a quote.

## Short answer

- **Satellite is feasible and cheap enough if the site sends summaries, not every
  reading.** The best fit is Blues' satellite parts, because the cellular uplink
  (FSD §33) already speaks to a Notecard: the same I2C API, the same Notehub routes
  to ThingsBoard and ntfy. A daily summary plus alerts costs about **$10 – 15 a year**
  in data. Every 10-minute reading over satellite costs **$400 – 2,000 a year**, so
  the full record stays in the flash history and comes home over BLE or later cellular.
- **LoRa is feasible and nearly free to run, but only where a gateway can be.** It
  pays off for a *cluster* of sites within a few km of one place that has power and
  internet (one gateway, ~$25 per node, $0 – 1 a year in data). For a single isolated
  site it helps only if public LoRaWAN coverage (Helium or TTN) is confirmed there.
- **Not recommended per sensor:** Iridium via RockBLOCK (monthly line rental and
  50-byte credits make it 10 – 20× Blues' price at our volumes), Starlink (20 – 40 W,
  so it is only a backhaul for a powered hub), Myriota (new stack, no Notehub path,
  pricing by quote). Swarm is no longer offered.

## What we would send

The device today takes a reading every 10 minutes (144 a day) and keeps a 24-byte
record in flash (FSD §27). Over cellular (§33) it sends JSON batches of up to 100
readings, which is far too big for satellite or LoRa. Packed binary forms:

| Message | Contents | Size |
|---|---|---|
| One reading | temp int16 (0.01 °C), RH uint8 (0.5 %), pressure uint16 (0.1 hPa), loop int16, flags | **8 bytes** (+2 for a time offset when batched) |
| Hourly batch | base time (4) + 6 readings × 10 | **64 bytes** |
| Daily summary | date, min/avg/max of T, RH, P and loop, battery mV, counts and flags | **~30 bytes** |
| Alert | alert code, value, time | **~10 bytes** (the ntfy title and text are built in the Notehub route, not sent) |

Email over satellite or LoRa is out: the §25 message is kilobytes. A route can
turn the daily summary into a short email if wanted.

## Options at a glance

| | Coverage | Payload per message | Latency | Extra hardware per site | Recurring cost |
|---|---|---|---|---|---|
| **Notecard for Skylo** (cell + Skylo satellite in one card) | US and other Skylo countries; GEO, needs a clear view of the southern sky | 50 – 256 bytes (50 billed minimum) | minutes | **$89**, plugs into the Notecarrier-X we have | 500 MB cellular for 10 years included; satellite 10 KB included, then **$0.00075/byte** |
| **Starnote for Skylo** (add-on to a cellular Notecard) | same | same | minutes | **$49** + Notecarrier XS **$15** (the X v1.3 can't hold a Starnote) | same |
| **Starnote for Iridium** | global, poles included; LEO, open sky | 10 – 10,000 bytes up, 340 down (10 billed minimum) | minutes | **$409 kit** (Starnote, Notecard Cell+WiFi, Notecarrier XI with supercaps, helical antenna, 5000 mAh LiPo) | 10 KB included, then **$0.00075/byte**, no monthly fee |
| Iridium SBD via RockBLOCK 9603 | global | up to 340 bytes, billed in 50-byte credits | ~1 min | ~$280 – 300 module *(approx.)* | line rental ~$15/month in months used + ~$0.05 – 0.15 per credit *(approx.)* |
| Myriota (UltraLite / HyperPulse) | global / US | 20 – 1,500 bytes | minutes to hours | module and dev kit by quote | plans from $0.99/month per device (UltraLite); HyperPulse not published |
| Starlink Mini | almost everywhere | IP, unlimited | ms | ~$200 – 500 dish *(approx.)*, 20 – 40 W | ~$5 – 50+/month depending on plan *(approx.)* |
| **LoRaWAN, public** (Helium, TTN) | only where a gateway hears you; check the coverage maps on site | US915: 11 bytes at SF10, 53 at SF9, 125+ at SF8 and faster | seconds | RAK3172-class module + antenna, ~$25 – 35 *(approx.)* | Helium **$0.00001 per 24 bytes**; TTN free within fair use (30 s airtime a day, 10 downlinks) |
| **LoRaWAN, own gateway** | 2 – 10 km per gateway depending on terrain | same | seconds | node ~$25 – 35, plus once per cluster a gateway ~$100 – 200 indoor or ~$300 – 600 outdoor *(approx.)* with WiFi/Ethernet/LTE backhaul | $0 on TTN; LTE backhaul for the gateway if no wired internet |
| **LoRa point-to-point** | 2 – 10 km line of sight | up to 255 bytes, our own protocol | seconds | SX1262 board at each end, ~$20 – 40 each *(approx.)*; the base is a second ESP32-S3 on WiFi | $0 |

## Data cost per site per year

Steady state, after the 10 KB that ships with each Blues satellite part (worth about
$7.50) is used up. Blues adds a few framing bytes per packet; add about 5 %.

| Pattern | Starnote / Notecard for Skylo | Starnote for Iridium | RockBLOCK *(approx.)* | LoRaWAN (Helium) |
|---|---|---|---|---|
| A. Daily summary + ~2 alerts a month | **~$15** | **~$9** | ~$220 | < $0.01 |
| B. Four 6-hour batches a day | ~$70 | ~$70 | ~$470 | < $0.10 |
| C. Hourly batch of 6 readings | ~$420 | ~$420 | ~$1,900 | ~$0.10 |
| D. Every reading on its own (144 a day) | ~$1,970 (50-byte minimum) | ~$390 | ~$5,400 | ~$0.53 |

Downlinks are billed too, so settings stay local (portal and BLE), never over satellite.

## Power

The board's own budget is about 9.5 mAh a day (1000 mAh for about 3.5 months, still an
estimate until sleep current is measured). Against that:

| Link | Per message | Per day |
|---|---|---|
| Notecard cellular, for comparison (§33.2) | one sync about 0.28 mAh | about 10 mAh at hourly syncs |
| LoRa at 20 dBm | SF7 ~56 ms ≈ 0.002 mAh; SF10 ~370 ms ≈ 0.012 mAh | 0.3 – 1.8 mAh for all 144 readings; module sleeps at ~2 µA |
| Skylo (NB-IoT NTN to GEO) | not published; longer transmissions than a cellular sync | measure on the bench; pattern A should stay small |
| Iridium (Certus 9704 on the Notecarrier XI) | not published; bursts are fed from supercaps, and the kit ships a 5000 mAh cell | measure on the bench before sizing the battery |

LoRa is the lightest by far. Satellite is fine at pattern A on the Notecard's own LiPo,
but needs a measurement before anything denser is promised.

## Fit with the firmware and shared components

**Satellite through Blues: small change.** The shared `notecard` component, the
`uplink_core` pure core and the Notehub routes of §33 stay as they are. What changes:

- `card.transport` set to `cell-ntn` (cellular first, satellite when it fails) or `ntn`
  for satellite-only sites.
- A compact `note.template` for one satellite notefile (for example `sat.qo`) holding
  the packed summary and alerts; Notehub expands it back to JSON, so the ThingsBoard
  and ntfy routes still receive JSON. The full `tb.qo` batches stay in a non-satellite
  notefile and wait for cellular, which is Blues' recommended two-template pattern.
- `uplink_core` gains the summary and alert packing and a rule for what goes on
  satellite; it stays pure and host-testable.
- A note over 256 bytes on Skylo is dropped silently, so a host test must pin
  every satellite note's size.

Hardware: **Notecard for Skylo** drops into the Notecarrier-X v1.3 we already have,
same I2C address 0x17, no wiring change. The Starnotes need a different carrier (XS
for Skylo, XI for Iridium), which also joins on Qwiic. One conflict to settle: §33.2
names the MBGLW Notecard as a `[user]` choice, and the "Choose vendor-neutral cellular
option" thread is weighing a move away from Blues. Satellite
through Blues deepens that dependency; satellite without Blues (RockBLOCK, Myriota)
means a new UART transport, a new backend path and much higher cost.

**LoRaWAN: new shared component, new backend path.**

- A new `lorawan_tx` in `esp-idf-components`. Simplest is a RAK3172 (STM32WLE5) over
  UART with AT commands, which keeps the LoRaWAN stack and its US915 certification in
  the module; the alternative is an SX1262 on SPI with RadioLib, which costs 4 – 6
  GPIOs and owns the stack ourselves. UART needs two GPIOs plus an optional reset,
  to be picked in `layout.md` against the RS232, loop and I2C pins.
- Backend: TTN or Helium decodes the uplink with a payload formatter and calls a
  webhook. ThingsBoard CE can't take that message directly, so it needs a small relay
  (or ThingsBoard PE's integration); ntfy alerts go the same way.
- No store-and-forward: unconfirmed uplinks can be lost, and TTN allows only 10
  downlinks a day, so the flash history stays the record and the §32 "uploaded up to"
  mark does not carry over.
- At SF10 (longest reach) US915 allows only 11 bytes and TTN's 30 s a day allows about
  one message every 20 – 30 minutes; at SF9 or faster every reading fits.

**LoRa point-to-point:** the same radio part on the node, plus a base station that is a
second ESP32-S3 on WiFi reusing `wifi_net`, the §32 ThingsBoard sender and `ntfy_tx`.
No vendor, no recurring cost, but we own the protocol (acks, retries, pairing,
encryption) and its tests.

## Recommendation

1. **Keep cellular (§33) as the default** for any site with coverage.
2. **For sites without cellular, use Blues satellite with pattern A** (daily summary
   plus alerts, about $15 a year): Notecard for Skylo in the US with a southern sky
   view, Starnote for Iridium where the sky is obstructed, at high latitude, or outside
   Skylo's countries. Full 10-minute data stays on the device and comes home over BLE
   on a visit or when cellular returns.
3. **Use LoRaWAN only for clusters**: three or more sites within a few km of a building
   with power and internet. One gateway on TTN, about $25 – 35 per node, no data bill.
   Choose point-to-point instead if there is only one or two nodes and a WiFi base nearby.
4. **Skip** RockBLOCK, Myriota and per-sensor Starlink for now. Starlink is worth it
   only as backhaul for a LoRa gateway at a powered hub with no other internet.

What decides it per site: whether cellular reaches it, whether other sites are within
a few km, which way the sky is open, and how fresh the data must be (daily summary or
near real time). With those answers the next step is a `/define` pass for a new FSD
section (satellite fallback on the §33 uplink, or a LoRa uplink) and a bench power
measurement of the chosen radio.

## Sources

- Blues: [Satellite constraints](https://dev.blues.io/connected-product-guidebook/firmware-best-practices-guide/#understanding-satellite-constraints),
  [packet sizes](https://dev.blues.io/support/diagnosing-satellite-connectivity-issues/#check-your-packet-sizes),
  [Notecarrier requirements](https://dev.blues.io/support/diagnosing-satellite-connectivity-issues/#notecarrier-requirements),
  [Starnote for Iridium](https://dev.blues.io/blog/pole-to-pole-starnote-for-iridium/),
  [Starnote for Iridium datasheet](https://dev.blues.io/datasheets/starnote-datasheet/starnote-for-iridium/),
  [Starnote for Iridium kit ($409)](https://shop.blues.com/products/starnote-for-iridium-kit),
  [Notecarrier XS ($15)](https://shop.blues.com/products/notecarrier-xs),
  [Two-template pattern](https://dev.blues.io/blog/get-ready-for-starnote/#2-minimize-how-often-you-send-data-over-satellite)
- [CNX Software: Notecard for Skylo ($89)](https://www.cnx-software.com/2026/03/11/blues-notecard-for-skylo-iot-module-offers-5g-ntn-satellite-nb-iot-lte-m-cellular-wifi-and-gnss-connectivity/)
- [Hackster: Starnote for Skylo ($49)](https://hackster.io/news/blues-partners-with-skylo-on-the-49-starnote-offering-low-cost-satellite-connectivity-for-the-iot-dc20b93ef287)
- [Ground Control: Iridium contract costs](https://docs.groundcontrol.com/iot/rockblock/web-services/iridium-contract-costs)
  (credit = 50 bytes; prices by quote)
- [Helium data credits](https://docs.helium.com/tokens/data-credit)
- [TTN fair use policy](https://thethingsnetwork.org/forum/t/the-things-network-fair-use-policy/47689)
- [Myriota FAQ](https://myriota.com/faqs/)
- Project: FSD §27, §32, §33; `docs/Method/project/blues-cellular.md`
