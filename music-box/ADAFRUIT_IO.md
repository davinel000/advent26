# Control the Music Box from Adafruit IO

You can open the private dashboard in a browser on any network. The ESP32 uses
its configured Wi-Fi and an outbound TLS connection to Adafruit IO. Once the
ESP32 has synchronized its clock, its saved event can play during an Internet
outage.

## 1. Prepare your account and the ESP32

1. Sign in at [Adafruit IO](https://io.adafruit.com/). Open [Feeds](https://io.adafruit.com/feeds)
   and create two private feeds in the default group. Use these exact keys:
   `musicbox-command` and `musicbox-status`.
   [Feed setup guide](https://learn.adafruit.com/adafruit-io-basics-feeds/creating-a-feed)
2. Open **My Key** in Adafruit IO to find the username and active key. Fill in
   `ADAFRUIT_IO_USERNAME` and `ADAFRUIT_IO_KEY` in your local
   `firmware/music_box/secrets.h`. Keep the existing Wi-Fi fields. Git ignores
   that file. [Where to find the key](https://learn.adafruit.com/adafruit-io-basics-airlift/adafruit-io-setup)
3. Upload `firmware/music_box/music_box.ino` using the same Arduino IDE settings
   as before. Open Serial Monitor at **115200 baud**. Look for `Clock synchronized`,
   then `Adafruit IO online. Ready for new commands.` The `musicbox-status` feed
   should get an `online` entry.

### Test directly on Adafruit IO Free

Adafruit lists a 15-minute interval for **Actions** on Free. The PowerShell
command uses the Adafruit IO HTTP API to publish a fresh command directly.
Direct feed writes use the 30-data-points-per-minute Free account limit.
[Plan comparison](https://io.adafruit.com/)
[Adafruit IO Create Data API](https://io.adafruit.com/api/docs/http.html#create-data)

Wait for `Adafruit IO online. Ready for new commands.` in Serial Monitor.
From the repository root, run one of these commands:

```powershell
.\music-box\tools\Send-MusicBoxCommand.ps1 play
.\music-box\tools\Send-MusicBoxCommand.ps1 stop
.\music-box\tools\Send-MusicBoxCommand.ps1 status
.\music-box\tools\Send-MusicBoxCommand.ps1 schedule '2026-12-05T08:30:00+01:00'
.\music-box\tools\Send-MusicBoxCommand.ps1 clear
```

Use a future time for `schedule`. Check `musicbox-status` for the device's
response. Each command uses the computer's current time and expires after
60 seconds. Add `-Preview` to inspect the request body locally without
loading credentials or contacting Adafruit IO.

## 2. Add one browser button and one Action

Adafruit lists Actions every 15 minutes on Free and every 5 seconds on IO+.
Use the direct feed test above to check repeated commands on Free.
[Adafruit IO plan comparison](https://io.adafruit.com/)

1. Create the private `musicbox-play` feed in the default group. Open
   [Dashboards](https://io.adafruit.com/dashboards) and create a private
   dashboard called **Music Box**.
2. Add a **Momentary Button** for `musicbox-play`. Set its caption to **Play**,
   press value to `1`, and release value to `0`. Add a **Stream** block for
   `musicbox-status` so you can see recent responses.
   [Dashboard blocks](https://learn.adafruit.com/adafruit-io-basics-dashboards?view=all)
3. In [Actions](https://io.adafruit.com/actions), create a **Play Music Box**
   Action. In **Triggers**, add **Data Matching** for `musicbox-play` and
   attach a **Compare Numbers Matcher** set to equal `1`. In **Actions**, add
   **Set Feed Value** for `musicbox-command`. Insert a **Text Template** with a
   **Multiline Text** input containing the following. Save and enable the Action.
   [Trigger](https://io.adafruit.com/actions-docs/blocks/triggers/when_data_matching.html)
   [Set Feed Value](https://io.adafruit.com/actions-docs/blocks/feeds/set_value.html)
   [Text Template](https://io.adafruit.com/actions-docs/blocks/text/template.html)

```liquid
{%- assign sent = 'now' | date: '%s' | plus: 0 -%}
{"v":1,"id":"play-{{ sent }}","op":"play","issued_at":{{ sent }},"expires_at":{{ sent | plus: 30 }},"melody":"jingle1"}
```

Once the online message appears in Serial Monitor, wait a couple of seconds and
press **Play**. Look for `playing` and then `completed` in `musicbox-status`. The
ESP32 publishes status updates at least five seconds apart to leave room for
command traffic within the shared rate limit. The status describes what the
firmware did. Listen to confirm the actual sound.
[Adafruit IO MQTT rate limits](https://io.adafruit.com/api/docs/mqtt#mqtt-api-rate-limiting)

## 3. Add Stop and schedule controls

For **Stop**, create a private `musicbox-stop` feed and repeat the Play button and
Action recipe using that feed. In the template, use `stop-{{ sent }}` for the ID,
`"op":"stop"`, and remove the `melody` field. Give Stop its own Action so it can run
while the separate Play Action is within Adafruit's trigger interval.

For schedules, create a private `musicbox-request` feed. Add a **Text Box** for
it to the dashboard. Create a third Action:

1. Add an **Any Data** trigger for `musicbox-request`.
2. Add **Set Variable** named `request`; insert **Get Feed Value** for
   `musicbox-request` as its input.
3. Below it add **Set Feed Value** for `musicbox-command`. Insert a **Text
   Template** with a **Multiline Text** input containing the following. Save and
   enable the Action.
   [Any Data trigger](https://io.adafruit.com/actions-docs/blocks/triggers/when_data.html)
   [Get Feed Value](https://io.adafruit.com/actions-docs/blocks/feeds/get_value.html)

```liquid
{%- assign sent = 'now' | date: '%s' | plus: 0 -%}
{%- assign words = vars.request | strip | split: ' ' -%}
{%- if words[0] == 'set' and words.size == 2 -%}
{"v":1,"id":"schedule-{{ sent }}","op":"schedule","issued_at":{{ sent }},"expires_at":{{ sent | plus: 30 }},"melody":"jingle1","fire_at":"{{ words[1] }}"}
{%- elsif vars.request == 'status' or vars.request == 'clear' -%}
{"v":1,"id":"{{ vars.request }}-{{ sent }}","op":"{{ vars.request }}","issued_at":{{ sent }},"expires_at":{{ sent | plus: 30 }}}
{%- else -%}
{}
{%- endif -%}
```

Send `set YYYY-MM-DDTHH:MM:SS+01:00` with a future date using the correct UTC
offset, such as `+01:00` for Berlin in winter. Use `+02:00` in summer or `Z` for
UTC. A new time replaces the previously saved event. Send `status` to see the
device's current schedule, or `clear` to cancel it. Wait for the device's response
before sending another request. The schedule is accepted once written to flash.
The dashboard's last status can remain visible when the device is offline.

## 4. Check it works away from home

- While the ESP32 stays on its current Wi-Fi, open Adafruit IO from another
  Internet connection. Send a fresh Play command and verify one jingle. Repeat
  with the PowerShell command on Free or within your plan's Action interval
  on the dashboard.
  Check for distinct IDs.
- Schedule an event a few minutes ahead. Compare the acknowledged UTC time
  with the Berlin time printed by the local USB `show` command. Clear it, then
  set another event. Disconnect the ESP32's network and verify that the second
  event still plays once.
- While the ESP32 is disconnected, publish a new Play command to the command
  feed. Reconnect the ESP32 and
  check that it stays silent until a fresh click. Re-publish a previous command
  value only for this controlled check and verify it causes no playback.
- Check the Actions **Previous Runs** and the `musicbox-command` feed if the
  dashboard click does not reach the device. Expect a fresh ID and Unix
  timestamps on each Play. If the feed has the command and the status reports
  a rejection, use the `error` field to identify the failing check.

## Command contract

The ESP32 subscribes to `<username>/feeds/musicbox-command` over MQTT 3.1.1
with TLS certificate and hostname verification. It publishes JSON status to
`<username>/feeds/musicbox-status`. Commands must be authenticated feed values
and carry a version, supported operation, ID, issue time, and expiry. The
ID is the operation and issue time joined by a hyphen. The firmware accepts
only strictly newer issue times per operation, which it persists across
reboots. Commands issued before or in the same second as the active MQTT
connection, retained messages, and commands outside their time window are
ignored or rejected. The maximum command lifetime is 120 seconds. The
templates use 30 seconds. Schedule commands support either an ISO timestamp
with explicit offset in `fire_at` or integer UTC seconds in `fire_at_utc`.
Play and Schedule use `jingle1` in this firmware.
