# Instructions for coding agents

This repository hosts **Advent 26**, a public QR advent-calendar website. The production site is deployed from `docs/` on GitHub Pages. **Never change `docs/`, existing QR tokens, URLs, `docs/messages.json`, or the advent content while working on the Music Box unless explicitly asked.** Do not change deployment settings.

## Music Box subproject
See, in order:
1. [music-box/README.md](music-box/README.md) — intent, hardware, boundaries
2. [music-box/ARCHITECTURE.md](music-box/ARCHITECTURE.md) — interfaces, failure modes, security
3. [music-box/TASKS.md](music-box/TASKS.md) — incremental deliverables and acceptance criteria

Implement one GitHub issue at a time in a focused branch/PR, with compile/test evidence. Keep new firmware, tests and build automation under `music-box/` except an explicitly scoped GitHub Actions workflow.

Key constraints:
- Hardware: classic ESP32 DevKit (ESP32-WROOM-32), Arduino-ESP32 **3.x**, I2S clock BCLK GPIO26, WS GPIO25, audio DOUT GPIO22; existing 5 W I2S amplifier and 10–20 W passive speaker (impedance/module to be confirmed). Microphone and feedback loop are NOT required for this subproject.
- Arduino-ESP32 3.x I2S API: use `driver/i2s_std.h` and avoid legacy Arduino-ESP32 2.x assumptions.
- No always-on computer on premises. Wi-Fi router and USB power only.
- Scheduled playback must run on the device after NTP sync, even through temporary Internet loss. Remote commands come via an authenticated cloud service; no inbound router port forwarding.
- Secrets MUST NOT enter this public repo, Github Pages, logs or client-side JS: Wi-Fi password, Adafruit IO key, personal addresses, and other access tokens. Provide `secrets.example.h` and ignore the real secrets file.
- Network operations must not block the audio callback/task. Never store / re-broadcast credentials in status.
- Do not autoplay old cloud commands on reconnect. Require unique IDs, time validity, duplicate detection, and acknowledgements.
- Limit audio amplitude; provide a physical test checklist. Hardware validation is done by the user, never claim a successful real-device test from CI.
- Use an **original or user-supplied** jingle; do not copy third-party recordings or copyrighted melody data into the repository.

Before coding, briefly explain intended file changes, risks and tests. Prefer simple, testable modules. Report any hardware- or cloud-only tests that cannot be performed.
