#include "audio.h"
#include "cloud.h"
#include "remote_notes.h"
#include "schedule.h"

#include <WiFi.h>
#include <esp_netif_sntp.h>
#include <nvs.h>

#include <atomic>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#ifndef WIFI_SSID_ALT
#define WIFI_SSID_ALT ""
#endif
#ifndef WIFI_PASSWORD_ALT
#define WIFI_PASSWORD_ALT ""
#endif

using music_box::DueAction;
using music_box::Schedule;
using music_box::ScheduleState;

namespace {

constexpr unsigned long kSerialBaud = 115200;
constexpr unsigned long kWifiRetryMs = 30000;
constexpr char kBerlinTimezone[] = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr char kScheduleKey[] = "schedule-v2";
constexpr char kOldScheduleKey[] = "schedule-v1";
constexpr char kAdventLastKey[] = "dec2026-last";
constexpr char kAdventOffKey[] = "dec2026-off";

struct OneShot {
  Schedule event;
  uint8_t day;
  music_box::Effect effect;
};
static_assert(sizeof(OneShot) == 24);

std::atomic<bool> clockSynced{false};
bool syncReported = false;
bool sntpReady = false;
bool readyChimed = false;
bool usingAlternate = false;
wl_status_t lastWifiStatus = WL_NO_SHIELD;
unsigned long lastWifiAttempt = 0;

Schedule schedule{};
uint8_t scheduledDay = music_box::kTestJingle;
music_box::Effect scheduledEffect = music_box::Effect::dry;
uint8_t lastAdventDay = 0;
bool adventEnabled = true;
nvs_handle_t storage = 0;
bool storageReady = false;
bool audioReady = false;
bool audioBusy = false;
char playingCommand[music_box::kCommandIdSize]{};
unsigned reportedCloudSession = 0;

char serialLine[48];
size_t serialLength = 0;
bool discardLine = false;

void onTimeSync(struct timeval*) {
  clockSynced.store(true);
}

void startClock() {
  setenv("TZ", kBerlinTimezone, 1);
  tzset();

  if (WIFI_SSID[0] == '\0' && WIFI_SSID_ALT[0] == '\0') {
    Serial.println("Wi-Fi credentials missing. Set secrets.h for scheduled playback.");
    return;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  usingAlternate = WIFI_SSID[0] == '\0';
  WiFi.begin(usingAlternate ? WIFI_SSID_ALT : WIFI_SSID,
             usingAlternate ? WIFI_PASSWORD_ALT : WIFI_PASSWORD);
  lastWifiAttempt = millis();

  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  config.start = false;
  config.wait_for_sync = false;
  config.sync_cb = onTimeSync;
  const esp_err_t result = esp_netif_sntp_init(&config);
  sntpReady = result == ESP_OK;
  if (!sntpReady) {
    Serial.printf("NTP setup failed (%s).\n", esp_err_to_name(result));
  }
}

void printWifiStatus(wl_status_t status) {
  if (WIFI_SSID[0] == '\0' && WIFI_SSID_ALT[0] == '\0') {
    Serial.println("Wi-Fi not configured. Fill WIFI_SSID and WIFI_PASSWORD in secrets.h.");
    return;
  }

  switch (status) {
    case WL_CONNECTED:
      Serial.println("Wi-Fi connected (IP address assigned).");
      break;
    case WL_NO_SSID_AVAIL:
      Serial.println("Wi-Fi network not found.");
      break;
    case WL_CONNECT_FAILED:
      Serial.println("Wi-Fi connection failed.");
      break;
    case WL_CONNECTION_LOST:
      Serial.println("Wi-Fi connection lost.");
      break;
    case WL_IDLE_STATUS:
      Serial.println("Wi-Fi connecting.");
      break;
    default:
      Serial.println("Wi-Fi disconnected.");
      break;
  }
}

void pollClock() {
  if (!sntpReady) {
    return;
  }

  const wl_status_t status = WiFi.status();
  if (status != lastWifiStatus) {
    printWifiStatus(status);
  }
  const bool connected = status == WL_CONNECTED;
  if (connected && lastWifiStatus != WL_CONNECTED) {
    const esp_err_t result = esp_netif_sntp_start();
    if (result != ESP_OK) {
      Serial.printf("NTP start failed (%s).\n", esp_err_to_name(result));
    }
  }
  lastWifiStatus = status;

  const unsigned long nowMs = millis();
  if (connected) {
    lastWifiAttempt = nowMs;
  } else if (nowMs - lastWifiAttempt >= kWifiRetryMs) {
    lastWifiAttempt = nowMs;
    if (WIFI_SSID_ALT[0] && WIFI_SSID[0]) {
      usingAlternate = !usingAlternate;
      WiFi.begin(usingAlternate ? WIFI_SSID_ALT : WIFI_SSID,
                 usingAlternate ? WIFI_PASSWORD_ALT : WIFI_PASSWORD);
    } else {
      WiFi.reconnect();
    }
  }

  if (!syncReported && clockSynced.load()) {
    syncReported = true;
    Serial.println("Clock synchronized. Send show for local time and schedule.");
  }
}

void startStorage() {
  esp_err_t result = nvs_open("music-box", NVS_READWRITE, &storage);
  if (result != ESP_OK) {
    Serial.printf("Schedule storage open failed (%s).\n", esp_err_to_name(result));
    return;
  }
  OneShot saved{};
  size_t expectedSize = sizeof(saved);
  size_t size = expectedSize;
  result = nvs_get_blob(storage, kScheduleKey, &saved, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    expectedSize = sizeof(saved.event);
    size = expectedSize;
    result = nvs_get_blob(storage, kOldScheduleKey, &saved.event, &size);
  }
  if (result == ESP_ERR_NVS_NOT_FOUND ||
      (result == ESP_OK && size == expectedSize && music_box::validSchedule(saved.event) &&
       saved.day <= 24 && (saved.effect == music_box::Effect::dry ||
                           saved.effect == music_box::Effect::echo))) {
    storageReady = true;
    schedule = saved.event;
    scheduledDay = saved.day;
    scheduledEffect = saved.effect;
    uint8_t disabled = 0;
    const auto lastResult = nvs_get_u8(storage, kAdventLastKey, &lastAdventDay);
    const auto offResult = nvs_get_u8(storage, kAdventOffKey, &disabled);
    adventEnabled = (lastResult == ESP_OK || lastResult == ESP_ERR_NVS_NOT_FOUND) &&
                    (offResult == ESP_OK || offResult == ESP_ERR_NVS_NOT_FOUND) &&
                    lastAdventDay <= 24 && !disabled;
    return;
  }
  Serial.println("Stored schedule could not be read. Scheduled playback disabled.");
  nvs_close(storage);
}

bool saveSchedule(const Schedule& updated, uint8_t day, music_box::Effect effect) {
  if (!storageReady) {
    Serial.println("Schedule storage unavailable.");
    return false;
  }
  const OneShot record{updated, day, effect};
  esp_err_t result = nvs_set_blob(storage, kScheduleKey, &record, sizeof(record));
  if (result == ESP_OK) {
    result = nvs_commit(storage);
  }
  if (result != ESP_OK) {
    storageReady = false;
    nvs_close(storage);
    Serial.printf("Schedule save failed (%s). Playback disabled until reset.\n",
                  esp_err_to_name(result));
    return false;
  }
  schedule = updated;
  scheduledDay = day;
  scheduledEffect = effect;
  return true;
}

void printBerlinTime(const char* label, int64_t utc) {
  const time_t timestamp = static_cast<time_t>(utc);
  struct tm localTime{};
  char formatted[48];
  if (localtime_r(&timestamp, &localTime) &&
      strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S %Z (%z)", &localTime) > 0) {
    Serial.printf("%s %s\n", label, formatted);
  }
}

void showSchedule() {
  printWifiStatus(WiFi.status());
  if (clockSynced.load()) {
    printBerlinTime("Clock", time(nullptr));
  } else {
    Serial.println("Waiting for clock sync.");
  }
  if (!storageReady) {
    Serial.println("Schedule storage unavailable.");
  } else if (schedule.state == ScheduleState::empty) {
    Serial.println("No scheduled event.");
  } else {
    const char* state = schedule.state == ScheduleState::pending ? "pending" :
                        schedule.state == ScheduleState::consumed ? "consumed" : "missed";
    Serial.printf("Event %lu (%s)\n", static_cast<unsigned long>(schedule.id), state);
    printBerlinTime("Scheduled for", schedule.fireAtUtc);
  }
}

const char* replaceSchedule(int64_t utc, uint8_t day = music_box::kTestJingle,
                            music_box::Effect effect = music_box::Effect::dry) {
  if (!clockSynced.load() || utc <= time(nullptr)) {
    return "not_future";
  }
  if (!storageReady) {
    return "schedule_storage_unavailable";
  }
  if (!music_box::hasMelody(day)) {
    return "melody_unavailable";
  }
  if (schedule.state == ScheduleState::pending && schedule.fireAtUtc == utc &&
      scheduledDay == day && scheduledEffect == effect) {
    return nullptr;
  }
  if (schedule.id == UINT32_MAX) {
    return "schedule_id_exhausted";
  }
  return saveSchedule({utc, schedule.id + 1, ScheduleState::pending}, day, effect)
             ? nullptr : "schedule_save_failed";
}

bool clearSchedule() {
  return storageReady && (schedule.state == ScheduleState::empty ||
                          saveSchedule({0, schedule.id, ScheduleState::empty},
                                       music_box::kTestJingle, music_box::Effect::dry));
}

void setSchedule(int64_t utc) {
  const char* error = replaceSchedule(utc);
  if (error) {
    Serial.printf("Schedule rejected (%s).\n", error);
  } else {
    showSchedule();
    music_box::reportCloud("", "schedule_updated", "", schedule, storageReady);
  }
}

void play(uint8_t day = music_box::kTestJingle, music_box::Effect effect = music_box::Effect::dry,
          const char* commandId = "") {
  if (!audioReady || audioBusy) {
    Serial.println("Audio unavailable or already playing.");
    return;
  }

  audioBusy = true;
  strcpy(playingCommand, commandId);
  if (day == music_box::kReadyTone) {
    Serial.println("Wi-Fi and clock ready. Playing five quiet beeps.");
  } else if (day == music_box::kTestJingle) {
    Serial.println("Playing test jingle.");
  } else {
    Serial.printf("Playing day%02u.\n", static_cast<unsigned>(day));
  }
  music_box::startPlayback(day, effect);
  music_box::reportCloud(playingCommand, "playing", "", schedule, storageReady);
}

void pollAudio() {
  music_box::PlaybackResult result{};
  if (!music_box::takePlaybackResult(result)) {
    return;
  }
  audioBusy = false;
  if (result.error != ESP_OK) {
    audioReady = false;
    Serial.printf("I2S write failed (%s). Reset to retry.\n", esp_err_to_name(result.error));
    music_box::reportCloud(playingCommand, "failed", "i2s_error", schedule, storageReady);
  } else {
    Serial.println(result.stopped ? "Stopped." : "Done. Send p to play again.");
    music_box::reportCloud(playingCommand, result.stopped ? "stopped" : "completed", "",
                           schedule, storageReady);
  }
  playingCommand[0] = '\0';
}

void pollSchedule() {
  if (!storageReady) {
    return;
  }
  const DueAction action = music_box::dueAction(schedule, clockSynced.load(), time(nullptr));
  if (action == DueAction::wait || (action == DueAction::play && audioBusy)) {
    return;
  }

  const bool shouldPlay = action == DueAction::play && audioReady && music_box::hasMelody(scheduledDay);
  Schedule updated = schedule;
  updated.state = shouldPlay ? ScheduleState::consumed : ScheduleState::missed;
  // Persist before playback so a reset cannot replay an event.
  if (saveSchedule(updated, scheduledDay, scheduledEffect)) {
    showSchedule();
    if (shouldPlay) {
      play(scheduledDay, scheduledEffect);
    } else {
      music_box::reportCloud("", "missed", "", schedule, storageReady);
    }
  }
}

void pollAdvent() {
  if (!adventEnabled || !storageReady || !clockSynced.load() || !audioReady || audioBusy) {
    return;
  }
  const auto now = time(nullptr);
  std::tm berlin{};
  if (!localtime_r(&now, &berlin)) {
    return;
  }
  const auto day = static_cast<uint8_t>(music_box::adventDayDue(berlin, lastAdventDay));
  if (!day || !music_box::hasMelody(day)) {
    return;
  }
  if (nvs_set_u8(storage, kAdventLastKey, day) != ESP_OK || nvs_commit(storage) != ESP_OK) {
    adventEnabled = false;
    Serial.println("Advent playback disabled because its date could not be saved.");
    music_box::reportCloud("", "failed", "advent_save_failed", schedule, storageReady);
    return;
  }
  lastAdventDay = day;
  play(day, music_box::Effect::dry);
}

void pollReadyChime() {
  if (!readyChimed && audioReady && !audioBusy &&
      lastWifiStatus == WL_CONNECTED && clockSynced.load()) {
    readyChimed = true;
    play(music_box::kReadyTone);
  }
}

void pollOnlineControl() {
  music_box::pollCloud(clockSynced.load() && lastWifiStatus == WL_CONNECTED);
  const auto session = music_box::cloudSession();
  if (session != reportedCloudSession) {
    reportedCloudSession = session;
    if (session) {
      Serial.println("Adafruit IO online. Ready for new commands.");
      music_box::reportCloud("", "online", "", schedule, storageReady);
    }
  }

  music_box::CloudCommand command{};
  const char* error = nullptr;
  if (!music_box::nextCloudCommand(command, error)) {
    return;
  }
  const char* state = "accepted";
  if (!error) {
    switch (command.op) {
      case music_box::Operation::play:
        if (!music_box::hasMelody(command.melodyDay)) {
          error = "melody_unavailable";
        } else if (audioReady && !audioBusy) {
          play(command.melodyDay, command.effect, command.id);
          return;
        } else {
          error = audioBusy ? "audio_busy" : "audio_unavailable";
        }
        break;
      case music_box::Operation::stop:
        if (audioBusy) {
          music_box::stopPlayback();
          state = "stopping";
        } else {
          state = "idle";
        }
        break;
      case music_box::Operation::schedule:
        error = replaceSchedule(command.fireAtUtc, command.melodyDay, command.effect);
        break;
      case music_box::Operation::clear:
        if (!clearSchedule()) {
          error = "schedule_storage_unavailable";
        }
        break;
      case music_box::Operation::status:
        state = audioBusy ? "playing" : "ready";
        break;
      case music_box::Operation::advent:
        if (!storageReady || nvs_set_u8(storage, kAdventOffKey, !command.adventEnabled) != ESP_OK ||
            nvs_commit(storage) != ESP_OK) {
          error = "advent_save_failed";
        } else {
          adventEnabled = command.adventEnabled;
          state = adventEnabled ? "advent_on" : "advent_off";
        }
        break;
      case music_box::Operation::notes:
        error = music_box::applyNotesCommand(command.melodyDay, command.notes, millis());
        break;
      case music_box::Operation::count:
        error = "invalid_command";
        break;
    }
  }
  if (error || command.op != music_box::Operation::status) {
    Serial.printf("Adafruit IO command %s.\n", error ? error : state);
  }
  music_box::reportCloud(command.id, error ? "rejected" : state, error ? error : "",
                         schedule, storageReady);
}

void handleCommand(std::string_view command) {
  if (command == "ready") {
    if (WiFi.status() == WL_CONNECTED && clockSynced.load()) {
      play(music_box::kReadyTone);
    } else {
      Serial.println("Wi-Fi and clock are not ready.");
    }
  } else if (command == "volume") {
    Serial.printf("Volume %u%%.\n", music_box::volumePercent());
  } else if (command == "echo") {
    play(music_box::kTestJingle, music_box::Effect::echo);
  } else if (command == "show") {
    showSchedule();
  } else if (command == "clear") {
    if (!clearSchedule()) {
      Serial.println("Schedule storage unavailable.");
      return;
    }
    showSchedule();
    music_box::reportCloud("", "schedule_cleared", "", schedule, storageReady);
  } else if (command.starts_with("set ")) {
    int64_t utc = 0;
    if (music_box::parseTimestamp(command.substr(4), utc)) {
      setSchedule(utc);
    } else {
      Serial.println("Use set YYYY-MM-DDTHH:MM:SSZ or include an explicit UTC offset.");
    }
  } else if (command.starts_with("in ")) {
    const auto text = command.substr(3);
    int seconds = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (result.ec == std::errc{} && result.ptr == text.data() + text.size() && seconds > 0) {
      if (clockSynced.load()) {
        setSchedule(time(nullptr) + seconds);
      } else {
        Serial.println("Wait for clock sync before scheduling.");
      }
    } else {
      Serial.println("Use in followed by a positive number of seconds.");
    }
  } else {
    Serial.println("Commands are p, echo, ready, volume, show, clear, in SECONDS or set TIMESTAMP.");
  }
}

void pollSerial() {
  for (size_t i = 0; i < sizeof(serialLine); ++i) {
    const int next = Serial.read();
    if (next < 0) {
      return;
    }
    if (next == '\n' || next == '\r') {
      if (discardLine) {
        Serial.println("Invalid or overlong command.");
      } else if (serialLength > 0) {
        handleCommand(std::string_view{serialLine, serialLength});
      }
      discardLine = false;
      serialLength = 0;
    } else if (!discardLine && serialLength == 0 && next == 'p') {
      play();
      return;
    } else if (next < ' ' || next > '~' || serialLength == sizeof(serialLine)) {
      discardLine = true;
    } else if (!discardLine) {
      serialLine[serialLength++] = static_cast<char>(next);
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  const esp_err_t result = music_box::startAudio();
  audioReady = result == ESP_OK;
  if (!audioReady) {
    Serial.printf("I2S setup failed (%s). Reset to retry.\n", esp_err_to_name(result));
  }

  startStorage();
  music_box::startRemoteNotes();
  startClock();
  music_box::startCloud();
  Serial.println("Ready. Send p to play or show for schedule status.");
}

void loop() {
  pollClock();
  music_box::pollVolume();
  pollAudio();
  pollSerial();
  pollSchedule();
  pollAdvent();
  pollOnlineControl();
  pollReadyChime();
  delay(1);
}
