# Changelog

## v0.2.0 (2026-11-16) — first test release, NOT yet verified on hardware

Firmware (Seeed XIAO ESP32C3):

- `ltc-node` — hybrid (default): online 30 min after power-on (Home Assistant,
  web UI, OTA; Wi-Fi set up via improv / captive portal), then low power.
  Generic binary: works with any phone hotspot.
- `ltc-node-lowpower` — no Wi-Fi at all. The hotspot name is baked in
  (`My iPhone` in the prebuilt binary): rebuild with your own `hotspot_ssid`,
  or use `ltc-node` and let it learn the hotspot.
- `ltc-node-online` — always online.

Features:

- SMPTE LTC 24 / 25 / 29.97 DF / 30 via RMT (bit-exact with libltc)
- Timing from phone-hotspot beacon TSF (passive, receive-only) or ESP-NOW master
- Duty-cycled sync, temperature-compensated free-run, model saved to flash
- ESP-NOW settings sync: fps / jam / resync interval / online time
- `tools/ltc_compare.py` to measure two nodes from one stereo recording

Flash: factory `.bin` with <https://web.esphome.io>, later updates with the
`.ota.bin` (hybrid / online editions) from the ESPHome dashboard or web UI.
