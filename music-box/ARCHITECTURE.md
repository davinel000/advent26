# Scheduled Music Box — architecture and reliability contract

Status: **design specification, not an implemented or hardware-validated firmware**.

## High-level flow

```text
Owner's phone, anywhere                      Home, unattended
┌─────────────────────┐      Internet       ┌───────────────────┐
│ Adafruit IO dashboard├─────────────────────►│ Adafruit IO MQTT │
└─────────────────────┘                     └─────────┬─────────┘
                                                     │ outbound TLS connection
                                Home Wi-Fi router ────┤
                                                     ▼
                                      ┌────────────────────────────┐
NTP server ─────(time sync over Wi-Fi)►│ ESP32: network + scheduler │
                                      │ settings NVS / dedup log   │
                                      │ synth + DSP / I2S writer   │
                                      └────────────┬───────────────┘
                                                   ▼
                                        5 W I2S amp → passive speaker
```

Adafruit IO is the recommended **MVP** broker/dashboard. A custom GitHub Pages interface is **phase 2 only** and must use authenticated server-side mediation, e.g. a Worker; never embed cloud MQTT credentials in published JavaScript. A static GitHub Pages site cannot by itself host a server process or privately safeguard broker API keys.

## Device responsibilities
- Start with amplifier output muted / silent; load NVS configuration; connect Wi-Fi; configure SNTP and MQTT.
- Keep local time as **UTC epoch seconds**. Use `Europe/Berlin` only when converting human-facing scheduled dates, accounting for CET/CEST DST changes. Serialize schedule timestamps as UTC or ISO 8601 with an explicit offset, not naive local times.
- Consider time **unknown** until NTP synchronizes after boot (ESP32 has no battery-backed RTC by default). Never trigger an unsynchronized schedule after a cold boot.
- Once synchronized, continue advancing local time during a brief network outage. Refresh NTP periodically; guard against backward and forward clock corrections.
- Store `schedule.id`, `schedule.fire_at_utc`, `schedule.melody_id`, `schedule.enabled`, and a persisted last-trigger record in NVS. Persist schedule *before* acknowledging a schedule update. Scheduled event is one-shot.
- Reject already-fired events; if device was powered off for its trigger window, **do not play hours later**. Adopt a documented grace window (suggestion: 120 seconds) and a safe skip/missed-event state.
- MQTT command handling **must not wait on I2S output**. Use a bounded thread-safe queue or short callbacks, a controlled audio task, and sensible reconnect backoff.
- Use the hardware I2S pins BCLK=26, WS=25, DOUT=22, 44,100 Hz starting point, 32-bit stereo slots if required by the existing working amp sketch. Confirm exact module before optimizing bit depth or slot format.
- No microphonic feedback loop is required. Keep the synthesis volume below a conservative software ceiling and provide ramped start/end to avoid pops.
- After a manual Play, return to the idle scheduled state. Support Stop while playing and never play concurrent melodies.

## MQTT command and telemetry contract (v1)
Use Adafruit IO feeds grouped under the owner's account. Suggested feed keys:
- `musicbox-command` — incoming command JSON
- `musicbox-status` — outgoing acknowledgments/state (publish **on changes only**, to respect free-plan rate limits)
A separate configuration feed may be added when needed; no heartbeat every second.

Example commands (schema illustrative; final payload validation required):
```json
{"v":1,"id":"cmd-20261205-001","op":"play","melody":"jingle1","issued_at":1796472000,"expires_at":1796472120}
{"v":1,"id":"cmd-20261205-002","op":"stop","issued_at":1796472030,"expires_at":1796472150}
{"v":1,"id":"cmd-20261205-003","op":"schedule","event_id":"evt-01","fire_at_utc":1796558400,"melody":"jingle1","issued_at":1796472000,"expires_at":1796472120}
```
The example timestamps are placeholders: testing must verify parsed UTC timestamps and expected local time.

Rules:
- Validate version, operation, IDs, requested melody, timestamp fields, payload length, ranges and authorization (through the secured MQTT connection).
- **No replay after reconnect**: on first startup do not treat an old feed value as a fresh Play. Use unique command ID, persisted dedup ID(s) and `expires_at`; when time is unsynchronized accept no time-sensitive commands. Do not assume standard MQTT retained behavior at Adafruit IO.
- Ack with `id`, `state` (`accepted|playing|completed|rejected|missed`), a safe error code and current UTC time when valid. A status message is a report, **not a guarantee of audible sound**.
- Cap publish rates; Adafruit IO currently limits the free tier to 30 data points per minute; check official API docs before implementing.
- Do not expose credentials, internal network details or private surprise text in acknowledgments or public logs.

## Persistence and failure scenarios
| Failure | Intended behavior |
| --- | --- |
| Wi-Fi lost after successful NTP sync | Local UTC schedule continues; reconnect in background; remote controls temporarily unavailable |
| Wi-Fi lost before first NTP sync | No scheduled playback; retry time sync |
| MQTT disconnect/reconnect | Exponential retry; no duplicated or stale Play |
| Reboot before scheduled time | Restore schedule from NVS; synchronize UTC before deciding |
| Reboot after successful play | Do not replay the same event; persisted event dedup |
| Power outage across play instant | Skip if outside defined grace window; report missed when online |
| Duplicate commands | Ack same ID without repeating side effects |
| Internet recovers late | Never execute expired Play commands |
| NTP clock jump / DST | Compare UTC epoch; avoid duplicate/missed local-hour ambiguity |
| Queue overload / sound task error | Fail safe, silence amp, recover and report error |

**Hardware caveat:** Loss of power during the tiny interval between starting a sound and persisting a trigger cannot be atomically solved with a single flash write. For surprise playback, choose **at-most-once** behavior: persist an event as consumed before starting audio. Tests must document the tradeoff (may skip sound if power dies at that moment).

## Security & deployment
- Public repository: keep `secrets.h`, `secrets.json`, `.env`, generated credentials and API tokens out of git. Provide a tracked `secrets.example.h` with dummy placeholders and a matching `.gitignore`.
- Prefer MQTT over TLS with certificate validation; never `setInsecure()` in shipped code.
- No inbound public ports, unauthenticated public Play endpoint, or hardcoded cloud key in GitHub Pages.
- Prefer Adafruit IO's hosted dashboard for remote control. Phase-2 custom interface requires trusted authentication & a secret-holding backend.
- A visitor should not be able to infer or trigger the surprise from public Advent 26 pages.

## DSP scope and memory
MVP: one-note-at-a-time oscillator (sine / triangle), ADSR-like fades, note + duration format, gain limiter, output silence when idle. Optional simple one-pole/two-pole low-pass and tremolo are cheap. A 500 ms mono 16-bit delay at 44.1 kHz requires ~44.1 kB; allocate only after checking heap headroom while Wi-Fi+TLS is connected. Consider short delay only in later issue.

## Test approach
- Automated: compile firmware for `esp32:esp32:esp32` using Arduino-ESP32 3.x and `arduino-cli`; unit-test pure UTC scheduler, command parser/idempotency and note parser as feasible without hardware.
- Hardware: low-volume playback and Stop, cold boot, 48+ hour unattended test, router reset, Wi-Fi outage, power interruption, DST/time change simulation, missing MQTT, multiple remote Play commands, speaker load/impedance confirmation.
- Always state what is unverified on physical hardware.

Official references: https://io.adafruit.com/api/docs/mqtt ; https://docs.espressif.com/projects/arduino-esp32/en/latest/ ; https://developers.openai.com/cookbook/examples/codex/iterating-development-workflows-with-codex
