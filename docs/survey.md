# LoRa site survey

[`firmware/survey`](../firmware/survey/main/main.cpp) checks
whether a site has LoRaWAN coverage before any LoRa hardware is designed in. It runs on
a **Heltec WiFi LoRa 32 V3** (ESP32-S3, SX1262, 0.96" SSD1306 OLED). It only
measures and is never released.

## What it measures

The board joins a LoRaWAN network in the US915 band (The Things Network or Helium, or
a private gateway on either). Each **round** sends 4-byte uplinks, each carrying a
**LinkCheckReq**. The network answers with a **LinkCheckAns**, which gives two numbers:

- **gateways**: how many gateways heard the uplink.
- **margin**: how many dB the best gateway received the uplink above the weakest
  signal it can still decode at that spreading factor.

The board also records the RSSI and SNR of the answer as it heard it. In the network
console, each uplink also shows every gateway's RSSI and SNR.

Two modes, set in `menuconfig`:

| Mode | A round sends | Downlinks per round |
|------|---------------|--------------------|
| **ladder** (default) | SF7 first, then SF8, SF9 and SF10, stopping at the first answer | at most 1 |
| **all** | one uplink at each of SF7 – SF10 | up to 4 |

Ladder mode respects The Things Network's fair use policy (30 s of airtime and 10
downlinks a day per device). Use all mode with Helium or a private gateway.

## Hardware

- The Heltec V3 in its **863 – 928 MHz** variant, with its 915 MHz antenna. **Screw on
  the antenna before powering the board**: transmitting without one can damage the SX1262.
- A USB-C cable, and for field use a USB power bank or a 3.7 V LiPo on the JST connector.
- Pins used (Heltec's V3 schematic): SX1262 NSS 8, SCK 9, MOSI 10, MISO 11, RST 12,
  BUSY 13, DIO1 14, with a 1.8 V TCXO on DIO3 and the RF switch on DIO2; OLED SDA 17,
  SCL 18, RST 21, powered by Vext (GPIO 36 low); the PRG key on GPIO 0; the LED on GPIO 35.

## 1. Register the device

**The Things Network** (free, community gateways). Check
[the TTN map](https://www.thethingsnetwork.org/map) and [TTN Mapper](https://ttnmapper.org)
for gateways near the site first.

1. Sign in to the [TTN console](https://console.cloud.thethings.network), choose the
   **North America (nam1)** cluster, and create an application.
2. **Register end device** → *Enter end device specifics manually*: frequency plan
   **United States 902-928 MHz, FSB 2 (used by TTN)**, LoRaWAN version **1.1.0**,
   regional parameters **RP002 1.0.4**. JoinEUI all zeros. Let the console generate the
   DevEUI, AppKey and NwkKey.
3. In the device's *General settings → Join settings*, tick **Resets join nonces**. The
   board keeps its DevNonce in flash, so this is only a safety net after a flash erase.

**Helium** (pay per message, about $0.00001 for each). Check
Helium's coverage explorer for hotspots near the site. Add the device in
a Helium console (for example a ChirpStack-based one) as **LoRaWAN 1.0.x, US915**, and
leave `CONFIG_SURVEY_NWK_KEY` empty: Helium uses only the AppKey.

## 2. Put the keys in and build

The keys never go into git. Copy the example and fill it in:

```bash
cd firmware/survey
cp sdkconfig.keys.example sdkconfig.keys   # ignored by git
# edit sdkconfig.keys: DevEUI, JoinEUI, AppKey, NwkKey (empty for 1.0.x)
rm -rf build                                # the keys are read only into a fresh build/sdkconfig
idf.py -B build -DSDKCONFIG=$PWD/build/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.keys" set-target esp32s3
idf.py -B build -DSDKCONFIG=$PWD/build/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.keys" build
```

Other settings are under *LoRa survey* in `idf.py ... menuconfig`: US915 sub-band (2),
transmit power (20 dBm), mode (ladder), and seconds between automatic rounds (0 means
only on a key press). Run the build with IDF v6.0.2 exported (CI uses `espressif/idf:v6.0.2`).
RadioLib 7.8.1 is fetched from GitHub on the first build.

## 3. Flash

The V3 has a USB-UART bridge (CP2102), so its port is `/dev/ttyUSB0` on Linux or `COMx`
on Windows. This is not the native USB of the T-Display.

```bash
idf.py -B build -DSDKCONFIG=$PWD/build/sdkconfig -p /dev/ttyUSB0 flash monitor
```

The board does not go on the bench slot: SLOT1 holds the T-Display of tdisplay-s3-amoled.

## 4. Survey a site

1. Power on outdoors at the spot where the sensor would go, at the height it would be
   mounted. The screen shows `joining, try N` until the join is accepted. A join that keeps
   failing is already a result: no gateway answered at any spreading factor. The board
   retries after 15 s, 30 s, 60 s and so on, up to 5 minutes. Press PRG to retry at once.
2. **Short press** on PRG runs a round. Each line shows one spreading factor:
   `SF7  m12dB 2gw -97` means 2 gateways heard the SF7 uplink, the best one with 12 dB to
   spare, and the answer arrived at −97 dBm. `no answer` means no gateway answered at
   that SF.
3. **Long press** (1 s) moves to the next spot. The spot number is in every uplink and in
   every log line. Note each spot's place, for example as a pin on your phone's map.
4. Run 3 – 5 rounds per spot. A single lost packet means little.

A 10-minute log over the serial port gives a record, for example with
`idf.py ... monitor | tee survey.log`:

```text
join,try,result,code,ms,devnonce
survey,spot,round,sf,result,margin_db,gateways,rssi_dbm,snr_db,fcnt_up,toa_ms
join,1,ok,-1118,5873,12
survey,1,1,7,ok,14,2,-96,6.5,0,46
```

## Reading the results

| Result | Meaning for a sensor at this spot |
|--------|-----------------------------------|
| SF7 answers with margin ≥ 10 dB | Solid: every reading can go out at SF7, with room for an enclosure and bad weather |
| SF7 answers with 0 – 10 dB margin | Usable. An enclosure and a smaller antenna cost about 3 – 6 dB, so plan on SF8 – SF9 |
| Only SF9 or SF10 answers | Marginal. The airtime limits allow a message only every 20 – 30 min on TTN, and payloads of at most 11 bytes at SF10 |
| No answer at any SF, or no join | No usable public coverage. Use a private gateway within a few km, or cellular or satellite (see [the feasibility note](lora-satellite-feasibility.md)) |

Each step up in spreading factor gains about 2.5 dB, so SF10 reaches about 7.5 dB further
than SF7. Margin is measured at the gateway on the uplink, which is the direction a
sensor uses. The downlink RSSI only shows that the answer came back.
