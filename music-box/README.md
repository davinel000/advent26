# Advent 26 — Remote Scheduled Music Box

## Why this exists
A surprise musical signal: a small USB-powered box waits quietly for days and plays a short melody at a chosen time or when its creator remotely presses Play from another Wi-Fi network. No resident PC, Raspberry Pi or local operator.

This is a **separate subproject within the existing public Advent 26 repository**. The existing 24-day QR calendar and GitHub Pages site under `docs/` must remain unchanged. This directory is **not** deployed by GitHub Pages.

## Hardware baseline (known)
- Classic wide **ESP32 DevKit / ESP32-WROOM-32**, powered by a stable USB adapter.
- Existing I2S amplifier, described as approximately **5 W**, with passive speaker approximately **10–20 W**. The exact amplifier part and speaker impedance must be checked before sustained tests.
- Existing verified wiring from prior audio sketch: **GPIO26 = BCLK, GPIO25 = WS/LRCLK, GPIO22 = DOUT**. A microphone formerly used on GPIO35 is not needed for this device.
- Arduino-ESP32 **3.x**, `ESP32 Dev Module` target, ESP-IDF 5 / `driver/i2s_std.h`. These pin assignments are a hypothesis to confirm on actual hardware before trusting playback.

## Issue 1 — Run the local sound test

The ESP32 waits silently for a `p` command over USB serial. Each command plays
one original jingle (about 1.4 seconds), then returns to silence. This test runs
offline and requires no credentials.

### Build and flash

Connect the ESP32 using a USB data cable with the amplifier disconnected.
In Arduino IDE, add the [Espressif boards index](https://espressif.github.io/arduino-esp32/package_esp32_index.json)
under **Preferences → Additional Boards Manager URLs**. Install **esp32 by Espressif
Systems 3.3.13** in Boards Manager. Open
[`firmware/music_box/music_box.ino`](firmware/music_box/music_box.ino), select
**ESP32 Dev Module** and the board's port, then click **Upload**.

With Arduino CLI **1.6.0**, run the following from `music-box/`:

```sh
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.13
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/music_box
arduino-cli board list
arduino-cli compile --fqbn esp32:esp32:esp32 --upload --port PORT firmware/music_box
```

Replace `PORT` with your board's port from `arduino-cli board list`.

### Wire and check the sound

Switch off power before wiring. Check the amplifier's datasheet for its supply,
gain, supported speaker impedance, and compatibility with 3.3 V logic and
44.1 kHz Philips I2S with two 32-bit slots. If the gain is adjustable, use the lowest
setting. The sketch caps the digital peak at about 3.1% of full scale. Actual
loudness depends on the amplifier and speaker.

| ESP32 | Amplifier |
| --- | --- |
| GPIO26 | BCLK |
| GPIO25 | WS / LRCLK |
| GPIO22 | DIN |
| GND | GND |

Wire the supply and speaker to the amplifier terminals specified in its datasheet.
Both I2S slots carry the same signal. The ESP32 provides BCLK and WS without MCLK.

1. Open Serial Monitor at **115200 baud**. Press reset if needed to see `Ready`.
   Listen for silence during startup and idle.
2. Send `p`. Listen for one quiet jingle with smooth note starts and ends.
   After `Done`, check that the speaker stays quiet.
3. Send `x` and check for silence. Send `p` again to check repeat playback.
   Power-cycle the board and check that it waits silently again.
4. Record the amp model, speaker impedance, supply and observed results in the PR.
   If there is no sound, check the wiring, I2S format and amplifier enable pin as
   applicable. An `I2S setup failed` or `I2S write failed` message identifies a driver
   error. `Done` confirms the firmware finished writing audio to the I2S driver.

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
