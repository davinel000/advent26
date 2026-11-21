#include "audio.h"
#include "schedule.h"

#include <WiFi.h>
#include <esp_netif_sntp.h>
#include <nvs.h>

#include <atomic>
#include <charconv>
#include <cstdlib>
#include <ctime>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

using music_box::DueAction;
using music_box::Schedule;
using music_box::ScheduleState;

namespace {

constexpr unsigned long kSerialBaud = 115200;
constexpr unsigned long kWifiRetryMs = 30000;
constexpr char kBerlinTimezone[] = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr char kScheduleKey[] = "schedule-v1";

std::atomic<bool> clockSynced{false};
bool syncReported = false;
bool sntpReady = false;
bool wifiWasConnected = false;
unsigned long lastWifiAttempt = 0;

Schedule schedule{};
nvs_handle_t storage = 0;
bool storageReady = false;
bool audioReady = false;

char serialLine[48];
size_t serialLength = 0;
bool discardLine = false;

void onTimeSync(struct timeval*) {
  clockSynced.store(true);
}

void startClock() {
  setenv("TZ", kBerlinTimezone, 1);
  tzset();

  if (WIFI_SSID[0] == '\0') {
    Serial.println("Wi-Fi credentials missing. Set secrets.h for scheduled playback.");
    return;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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

void pollClock() {
  if (!sntpReady) {
    return;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    const esp_err_t result = esp_netif_sntp_start();
    if (result != ESP_OK) {
      Serial.printf("NTP start failed (%s).\n", esp_err_to_name(result));
    }
  }
  wifiWasConnected = connected;

  const unsigned long nowMs = millis();
  if (connected) {
    lastWifiAttempt = nowMs;
  } else if (nowMs - lastWifiAttempt >= kWifiRetryMs) {
    lastWifiAttempt = nowMs;
    WiFi.reconnect();
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
  size_t size = sizeof(schedule);
  result = nvs_get_blob(storage, kScheduleKey, &schedule, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND ||
      (result == ESP_OK && size == sizeof(schedule) && music_box::validSchedule(schedule))) {
    storageReady = true;
    return;
  }
  Serial.println("Stored schedule could not be read. Scheduled playback disabled.");
  nvs_close(storage);
}

bool saveSchedule(const Schedule& updated) {
  if (!storageReady) {
    Serial.println("Schedule storage unavailable.");
    return false;
  }
  esp_err_t result = nvs_set_blob(storage, kScheduleKey, &updated, sizeof(updated));
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

void setSchedule(int64_t utc) {
  if (!clockSynced.load() || utc <= time(nullptr)) {
    Serial.println("Schedule requires a synced clock and a future time.");
    return;
  }
  if (!storageReady) {
    Serial.println("Schedule storage unavailable.");
    return;
  }
  if (schedule.state == ScheduleState::pending && schedule.fireAtUtc == utc) {
    showSchedule();
    return;
  }
  if (schedule.id == UINT32_MAX) {
    Serial.println("Schedule ID space exhausted.");
    return;
  }
  if (saveSchedule({utc, schedule.id + 1, ScheduleState::pending})) {
    showSchedule();
  }
}

void playTest() {
  if (!audioReady) {
    Serial.println("Audio unavailable. Reset to retry.");
    return;
  }
  Serial.println("Playing test jingle.");
  const esp_err_t result = music_box::playJingle();
  for (int pending = Serial.available(); pending > 0; --pending) {
    Serial.read();
  }
  if (result == ESP_OK) {
    Serial.println("Done. Send p to play again.");
  } else {
    audioReady = false;
    Serial.printf("I2S write failed (%s). Reset to retry.\n", esp_err_to_name(result));
  }
}

void pollSchedule() {
  if (!storageReady) {
    return;
  }
  const bool synced = clockSynced.load();
  const DueAction action = music_box::dueAction(schedule, synced, time(nullptr));
  if (action == DueAction::wait) {
    return;
  }

  const bool shouldPlay = action == DueAction::play && audioReady;
  Schedule updated = schedule;
  updated.state = shouldPlay ? ScheduleState::consumed : ScheduleState::missed;
  // Persist before playback so a reset cannot replay an event.
  if (saveSchedule(updated)) {
    showSchedule();
    if (shouldPlay) {
      playTest();
    }
  }
}

void handleCommand(std::string_view command) {
  if (command == "show") {
    showSchedule();
  } else if (command == "clear") {
    if (!storageReady || schedule.state != ScheduleState::empty) {
      if (!saveSchedule({0, schedule.id, ScheduleState::empty})) {
        return;
      }
    }
    showSchedule();
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
    Serial.println("Commands are p, show, clear, in SECONDS or set TIMESTAMP.");
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
      playTest();
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
  startClock();
  Serial.println("Ready. Send p to play or show for schedule status.");
}

void loop() {
  pollClock();
  pollSerial();
  pollSchedule();
  delay(1);
}
