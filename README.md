# LoRa

Work on LoRa links for the remote sensor sites of
[tdisplay-s3-amoled](https://github.com/solchjg/tdisplay-s3-amoled).

- Test board: Heltec WiFi LoRa 32 V3 (ESP32-S3, SX1262, 863 – 928 MHz, 0.96" SSD1306 OLED).
- [docs/lora-satellite-feasibility.md](docs/lora-satellite-feasibility.md): the feasibility
  and cost analysis of LoRa and satellite links for those sites.
- [docs/site-scanner-plan.md](docs/site-scanner-plan.md): the plan for a site
  connectivity scanner on the Heltec that rates each possible link at a site.
- [docs/scanner-setup.md](docs/scanner-setup.md): building, flashing and using the
  scanner (phase 1: WiFi, The Things Network and Helium). The firmware is
  `firmware/scanner`, the phone page `web/index.html`, the pure core
  `components/scan_core` (host tests in `tests/host`).
- The LoRaWAN site survey program for the Heltec is `commissioning/lora-survey` in
  tdisplay-s3-amoled (`docs/Method/project/lora-survey.md` there).
