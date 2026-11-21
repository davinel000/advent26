# Advent 26 — Remote Scheduled Music Box

## Why this exists
A surprise musical signal: a small USB-powered box waits quietly for days and plays a short melody at a chosen time or when its creator remotely presses Play from another Wi-Fi network. No resident PC, Raspberry Pi or local operator.

This is a **separate subproject within the existing public Advent 26 repository**. The existing 24-day QR calendar and GitHub Pages site under `docs/` must remain unchanged. This directory is **not** deployed by GitHub Pages.

## Hardware baseline (known)
- Classic wide **ESP32 DevKit / ESP32-WROOM-32**, powered by a stable USB adapter.
- Existing I2S amplifier, described as approximately **5 W**, with passive speaker approximately **10–20 W**. The exact amplifier part and speaker impedance must be checked before sustained tests.
- Existing verified wiring from prior audio sketch: **GPIO26 = BCLK, GPIO25 = WS/LRCLK, GPIO22 = DOUT**. A microphone formerly used on GPIO35 is not needed for this device.
- Arduino-ESP32 **3.x**, `ESP32 Dev Module` target, ESP-IDF 5 / `driver/i2s_std.h`. These pin assignments are a hypothesis to confirm on actual hardware before trusting playback.

## What MVP should do
1. Connect to a configured 2.4 GHz home Wi-Fi, never needing a resident computer.
2. Obtain UTC time from NTP; use the local clock between synchronizations.
3. Store a **one-shot scheduled event** in nonvolatile settings; play at the correct time once.
4. Receive **remote Play / Stop / Schedule / Status** commands over TLS via **Adafruit IO MQTT** (cloud dashboard as MVP).
5. Generate a short original note sequence on board and output through the existing I2S amp. Add volume cap, and optionally low-pass/tremolo or a short delay *after* reliable playback works.
6. Recover safely from Wi-Fi / MQTT interruptions and restarts, with visible status / acknowledgments.
7. Keep all passwords and broker keys out of git and the public calendar.

## Out of scope for MVP
- Live audio streaming / remotely uploading large WAV or MP3 files.
- A publicly exposed direct web server on the ESP32.
- Replacing or editing calendar QR pages, content or their URLs.
- Audio feedback/microphone performance features from the earlier Modal Feedback sketch.
- A new paid server, always-on home machine, local RTC module or SD card.

## Recommended rollout
1. **Hardware smoke test:** synthesize the original jingle offline over I2S at low volume.
2. **Time and schedule:** NTP, persisted UTC event, exactly-once behavior.
3. **Cloud command path:** secured Adafruit IO MQTT, dashboard Play and status/ack.
4. **Reliability:** reconnect/reboot tests, build checks, no duplicate plays.
5. **Optional effects** and, later, an authenticated custom UI.

See [ARCHITECTURE.md](ARCHITECTURE.md) and [TASKS.md](TASKS.md). The GitHub Issues are intended as self-contained Codex tasks. Work on an isolated branch and submit a PR; compile/test reports are mandatory.
