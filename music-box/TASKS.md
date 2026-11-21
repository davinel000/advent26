# Codex work breakdown (one issue / PR at a time)

The first release is a **reliability-first surprise timer**, not a general-purpose audio player. Issues should be implemented in order. Read `../AGENTS.md`, `README.md`, and `ARCHITECTURE.md` before changing code. Existing `../docs/` GitHub Pages files must not change.

## Issue 1 — Compileable firmware skeleton + I2S sound test
**Goal:** Boot classic ESP32 under Arduino-ESP32 3.x, generate short original test jingle using pins 26/25/22 and produce silence when idle.

**Deliverables:** `firmware/music_box/music_box.ino`, small audio modules if justified, `secrets.example.h`, `.gitignore`, simple flash instructions, exact Arduino core version notes.

**Acceptance:** `arduino-cli compile --fqbn esp32:esp32:esp32 firmware/music_box` works with Arduino ESP32 core 3.x; no microphone code; no boot melody unless explicitly configured; Play Test at low capped volume; audio writes do not loop indefinitely. Hardware output remains user-verified.

## Issue 2 — NTP clock + one-shot schedule + NVS
**Goal:** Keep real time with Wi-Fi/NTP, persist an ISO-with-offset/UTC one-shot event and avoid duplicate execution.

**Acceptance:** No audio before time is valid after boot; scheduled playback at UTC target; persistent consumed event; no replays after restart; offline continuity after time sync; explicit missed-event/grace behavior and test cases for reboot/network outage/DST. Provide serial diagnostics without secrets.

## Issue 3 — Adafruit IO MQTT/TLS remote Play / Stop / Schedule
**Goal:** Control ESP32 from another Wi-Fi network via cloud; no local computer.

**Acceptance:** TLS certificate verification; reconnect/backoff; credentials in ignored secret file; command ID, expiry validation, persisted dedup, bounded payload; Play/Stop/Schedule work; status acknowledgments; old stored feed data cannot re-trigger sound. Do not exceed free service publish limit. Provide Adafruit IO account/dashboard setup instructions; do not create actual cloud credentials or claim live cloud verification.

## Issue 4 — CI, simulation and robustness
**Goal:** Build automatically and verify state machines without the hardware.

**Acceptance:** GitHub Actions firmware compile pinned to Arduino-ESP32 3.x; host-side tests for time/persistence logic where possible, malformed/duplicate/expired MQTT messages, schedules spanning DST, network reconnect and command queue behavior. Add a manual 48-hour unattended test checklist and measured results template. CI green does not equal real-world reliability.

## Issue 5 — Optional synth and effects + next-step UI
**Goal:** Add selectable waveform, low-pass, tremolo, optional short echo; expose a few guarded effect parameters through existing cloud commands.

**Acceptance:** No artifacts at boot/stop; no delay-memory failure when TLS is connected; CPU/heap headroom documented; audibly useful default settings; no blocking of NTP/MQTT; safe max output amplitude. For a custom GitHub Pages editor, produce **a separate design** requiring a server-side credential holder rather than publishing Adafruit API keys.

## Suggested Codex initial prompt
> Work on GitHub repo `davinel000/advent26`, issue 1 of the Music Box subproject. Read `AGENTS.md`, `music-box/README.md`, `music-box/ARCHITECTURE.md`, and `music-box/TASKS.md`. Keep `docs/` completely unchanged. Implement the minimal Arduino-ESP32 3.x I2S jingle test, compile it in the environment if possible, and provide a PR with exact changes, build output, remaining hardware assumptions and manual tests. Do not add passwords, audio recordings or unrelated features.

## Definition of done
A task is complete only with a focused PR, code reviewable on GitHub, build/test evidence (or named blockers), updated documentation, and explicit list of tests that still require the actual ESP32 and speaker.
