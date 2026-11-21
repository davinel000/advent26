#include "cloud.h"

#include <Arduino.h>
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <mqtt_client.h>
#include <nvs.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#ifndef ADAFRUIT_IO_USERNAME
#define ADAFRUIT_IO_USERNAME ""
#endif
#ifndef ADAFRUIT_IO_KEY
#define ADAFRUIT_IO_KEY ""
#endif

namespace music_box {
namespace {

constexpr int kMaxPayload = 240;
constexpr int64_t kMaxCommandLifetime = 120;
constexpr int64_t kMaxEpochSeconds = 253402300799;
constexpr int64_t kClockSkewSeconds = 3;
constexpr unsigned long kRetryMinMs = 15000;
constexpr unsigned long kRetryMaxMs = 120000;
constexpr unsigned long kPublishIntervalMs = 5000;
constexpr char kReplayKey[] = "issued-v1";
constexpr const char* kOperations[] = {"play", "stop", "schedule", "clear", "status"};
static_assert(std::size(kOperations) == static_cast<size_t>(Operation::count));

struct Packet {
  unsigned session;
  int64_t connectedAt;
  size_t length;
  char payload[kMaxPayload + 1];
};

struct Status {
  unsigned session;
  char payload[256];
};

esp_mqtt_client_handle_t client = nullptr;
QueueHandle_t inbox = nullptr;
QueueHandle_t outbox = nullptr;
std::atomic<unsigned> activeSession{0};
std::atomic<unsigned> failures{0};

char commandTopic[96];
char statusTopic[96];
int64_t connectedAt = 0;
unsigned sessionNumber = 0;
int subscription = -1;

bool started = false;
unsigned observedFailures = 0;
unsigned long retryDelay = kRetryMinMs;
unsigned long failureAt = 0;
unsigned long lastPublish = 0;

nvs_handle_t replayStorage = 0;
bool replayReady = false;
int64_t acceptedAt[static_cast<size_t>(Operation::count)]{};

bool readInteger(const cJSON* value, int64_t& result) {
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
      value->valuedouble < 0 || value->valuedouble > kMaxEpochSeconds ||
      std::trunc(value->valuedouble) != value->valuedouble) {
    return false;
  }
  result = static_cast<int64_t>(value->valuedouble);
  return true;
}

bool parseCommand(const Packet& packet, CloudCommand& command) {
  // The protocol is a flat object. Bound parser depth and reject embedded NULs.
  const std::string_view raw(packet.payload, packet.length);
  const size_t opening = raw.find('{');
  if (opening == raw.npos || raw.find('{', opening + 1) != raw.npos ||
      raw.find_first_of("[]") != raw.npos || raw.find('\0') != raw.npos ||
      raw.find("\\u0000") != raw.npos) {
    return false;
  }
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root{
      cJSON_ParseWithLengthOpts(packet.payload, packet.length + 1, nullptr, true), cJSON_Delete};
  if (!cJSON_IsObject(root.get())) {
    return false;
  }
  const auto field = [&root](const char* key) {
    return cJSON_GetObjectItemCaseSensitive(root.get(), key);
  };

  int64_t version = 0;
  if (!readInteger(field("v"), version) || version != 1 ||
      !readInteger(field("issued_at"), command.issuedAt) ||
      !readInteger(field("expires_at"), command.expiresAt)) {
    return false;
  }
  const char* op = cJSON_GetStringValue(field("op"));
  const char* id = cJSON_GetStringValue(field("id"));
  if (!op || !id) {
    return false;
  }
  const auto operation = std::find_if(std::begin(kOperations), std::end(kOperations),
                                      [op](const char* name) { return strcmp(name, op) == 0; });
  if (operation == std::end(kOperations)) {
    return false;
  }
  command.op = static_cast<Operation>(operation - std::begin(kOperations));
  snprintf(command.id, sizeof(command.id), "%s-%lld", op,
           static_cast<long long>(command.issuedAt));
  if (strcmp(id, command.id) != 0) {
    command.id[0] = '\0';
    return false;
  }

  int fields = 5;
  if (command.op == Operation::play || command.op == Operation::schedule) {
    const char* melody = cJSON_GetStringValue(field("melody"));
    if (!melody) {
      return false;
    }
    command.melodyDay = kTestJingle;
    if (strcmp(melody, "jingle1") != 0) {
      const std::string_view name(melody);
      if (command.op != Operation::play || name.size() != 5 || !name.starts_with("day") ||
          name[3] < '0' || name[3] > '2' || name[4] < '0' || name[4] > '9') {
        return false;
      }
      command.melodyDay = (name[3] - '0') * 10 + name[4] - '0';
      if (command.melodyDay < 1 || command.melodyDay > 24) {
        return false;
      }
    }
    ++fields;
  }
  command.effect = Effect::dry;
  if (command.op == Operation::play) {
    const cJSON* effect = field("effect");
    if (effect) {
      const char* name = cJSON_GetStringValue(effect);
      if (!name || (strcmp(name, "dry") != 0 && strcmp(name, "echo") != 0)) {
        return false;
      }
      command.effect = strcmp(name, "echo") == 0 ? Effect::echo : Effect::dry;
      ++fields;
    }
  }
  if (command.op == Operation::schedule) {
    const char* timestamp = cJSON_GetStringValue(field("fire_at"));
    if (timestamp) {
      if (!parseTimestamp(timestamp, command.fireAtUtc)) {
        return false;
      }
    } else if (!readInteger(field("fire_at_utc"), command.fireAtUtc)) {
      return false;
    }
    ++fields;
  }
  // The exact size together with the required fields rules out extra or duplicate keys.
  const cJSON* child = nullptr;
  cJSON_ArrayForEach(child, root.get()) {
    --fields;
  }
  return fields == 0;
}

void onMqttEvent(void*, esp_event_base_t, int32_t eventId, void* data) {
  const auto event = static_cast<esp_mqtt_event_handle_t>(data);
  switch (eventId) {
    case MQTT_EVENT_CONNECTED:
      connectedAt = time(nullptr);
      subscription = esp_mqtt_client_subscribe_single(event->client, commandTopic, 1);
      break;
    case MQTT_EVENT_SUBSCRIBED:
      if (subscription >= 0 && event->msg_id == subscription &&
          (!event->data || event->data_len == 0 ||
           static_cast<uint8_t>(event->data[0]) < 0x80)) {
        activeSession.store(++sessionNumber);
      }
      break;
    case MQTT_EVENT_DISCONNECTED:
      activeSession.store(0);
      ++failures;
      break;
    case MQTT_EVENT_DATA: {
      const auto session = activeSession.load();
      // The buffer is larger than any allowed packet. Reject fragments, retained values and other feeds.
      if (!session || event->retain || event->current_data_offset != 0 ||
          event->data_len <= 0 || event->data_len > kMaxPayload ||
          event->data_len != event->total_data_len ||
          event->topic_len != static_cast<int>(strlen(commandTopic)) ||
          memcmp(event->topic, commandTopic, event->topic_len) != 0) {
        break;
      }
      Packet packet{session, connectedAt, static_cast<size_t>(event->data_len), {}};
      memcpy(packet.payload, event->data, packet.length);
      xQueueSend(inbox, &packet, 0);
      break;
    }
    default:
      break;
  }
}

const char* checkReplay(const CloudCommand& command) {
  if (!replayReady) {
    return "replay_storage_unavailable";
  }
  const auto index = static_cast<size_t>(command.op);
  if (command.issuedAt <= acceptedAt[index]) {
    return "duplicate_or_older";
  }
  acceptedAt[index] = command.issuedAt;
  if (nvs_set_blob(replayStorage, kReplayKey, acceptedAt, sizeof(acceptedAt)) != ESP_OK ||
      nvs_commit(replayStorage) != ESP_OK) {
    replayReady = false;
    return "replay_storage_error";
  }
  return nullptr;
}

}  // namespace

void startCloud() {
  if (ADAFRUIT_IO_USERNAME[0] == '\0' || ADAFRUIT_IO_KEY[0] == '\0') {
    Serial.println("Adafruit IO not configured. Local commands still available.");
    return;
  }
  const std::string_view username(ADAFRUIT_IO_USERNAME);
  if (username.size() > 60 || username.find_first_not_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != username.npos) {
    Serial.println("Invalid Adafruit IO username in secrets.h.");
    return;
  }
  snprintf(commandTopic, sizeof(commandTopic), "%s/feeds/musicbox-command", ADAFRUIT_IO_USERNAME);
  snprintf(statusTopic, sizeof(statusTopic), "%s/feeds/musicbox-status", ADAFRUIT_IO_USERNAME);

  if (nvs_open("musicbox-cloud", NVS_READWRITE, &replayStorage) == ESP_OK) {
    size_t size = sizeof(acceptedAt);
    const auto result = nvs_get_blob(replayStorage, kReplayKey, acceptedAt, &size);
    replayReady = result == ESP_ERR_NVS_NOT_FOUND ||
                  (result == ESP_OK && size == sizeof(acceptedAt) &&
                   std::all_of(std::begin(acceptedAt), std::end(acceptedAt), [](int64_t value) {
                     return value >= 0 && value <= kMaxEpochSeconds;
                   }));
  }
  if (!replayReady) {
    Serial.println("Adafruit IO disabled because command replay storage is unavailable.");
    return;
  }
  inbox = xQueueCreate(4, sizeof(Packet));
  outbox = xQueueCreate(6, sizeof(Status));
  if (!inbox || !outbox) {
    Serial.println("Adafruit IO queue setup failed.");
    return;
  }

  esp_mqtt_client_config_t config{};
  config.broker.address.uri = "mqtts://io.adafruit.com:8883";
  config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  config.credentials.username = ADAFRUIT_IO_USERNAME;
  config.credentials.authentication.password = ADAFRUIT_IO_KEY;
  config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  config.network.disable_auto_reconnect = true;
  config.network.timeout_ms = 5000;
  config.buffer.size = 1024;
  config.outbox.limit = 1024;
  client = esp_mqtt_client_init(&config);
  if (!client || esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onMqttEvent, nullptr) != ESP_OK) {
    Serial.println("Adafruit IO setup failed.");
    client = nullptr;
  }
}

void pollCloud(bool canConnect) {
  if (!client) {
    return;
  }
  const auto now = millis();
  if (!started && canConnect) {
    started = esp_mqtt_client_start(client) == ESP_OK;
    if (!started) {
      Serial.println("Adafruit IO could not start. Reset to retry.");
      client = nullptr;
    }
    return;
  }
  if (!started) {
    return;
  }

  const unsigned session = activeSession.load();
  if (!session) {
    const unsigned currentFailures = failures.load();
    if (observedFailures != currentFailures) {
      observedFailures = currentFailures;
      failureAt = now;
      Serial.println("Adafruit IO MQTT disconnected. Will retry.");
    }
    if (canConnect && currentFailures > 0 && now - failureAt >= retryDelay) {
      failureAt = now;
      esp_mqtt_client_reconnect(client);
      retryDelay = std::min(retryDelay * 2, kRetryMaxMs);
    }
    return;
  }
  retryDelay = kRetryMinMs;

  Status status{};
  while (xQueuePeek(outbox, &status, 0) == pdTRUE && status.session != session) {
    xQueueReceive(outbox, &status, 0);
  }
  if (now - lastPublish >= kPublishIntervalMs && xQueuePeek(outbox, &status, 0) == pdTRUE) {
    lastPublish = now;
    if (esp_mqtt_client_enqueue(client, statusTopic, status.payload, 0, 0, 0, true) >= 0) {
      xQueueReceive(outbox, &status, 0);
    }
  }
}

unsigned cloudSession() {
  return activeSession.load();
}

bool nextCloudCommand(CloudCommand& command, const char*& error) {
  Packet packet{};
  if (!inbox || xQueueReceive(inbox, &packet, 0) != pdTRUE) {
    return false;
  }
  const int64_t now = time(nullptr);
  if (packet.session != activeSession.load()) {
    error = "session_changed";
  } else if (!parseCommand(packet, command)) {
    error = "invalid_command";
  } else if (command.issuedAt <= packet.connectedAt) {
    error = "before_session";
  } else if (command.issuedAt > now + kClockSkewSeconds || command.expiresAt <= now ||
             command.expiresAt <= command.issuedAt ||
             command.expiresAt - command.issuedAt > kMaxCommandLifetime) {
    error = "invalid_time_window";
  } else {
    error = checkReplay(command);
  }
  return true;
}

void reportCloud(const char* id, const char* state, const char* error,
                 const Schedule& schedule, bool storageReady) {
  const unsigned session = activeSession.load();
  if (!outbox || !session) {
    return;
  }
  const char* scheduleState = !storageReady ? "unavailable" :
                              schedule.state == ScheduleState::pending ? "pending" :
                              schedule.state == ScheduleState::consumed ? "consumed" :
                              schedule.state == ScheduleState::missed ? "missed" : "empty";
  const bool hasEvent = storageReady && schedule.state != ScheduleState::empty;
  char berlinTime[32]{};
  if (hasEvent) {
    const time_t fireAt = static_cast<time_t>(schedule.fireAtUtc);
    struct tm local{};
    if (localtime_r(&fireAt, &local)) {
      strftime(berlinTime, sizeof(berlinTime), "%Y-%m-%dT%H:%M:%S%z", &local);
    }
  }
  Status status{session, {}};
  const int length = snprintf(status.payload, sizeof(status.payload),
      "{\"id\":\"%s\",\"state\":\"%s\",\"error\":\"%s\",\"utc\":%lld,"
      "\"schedule\":\"%s\",\"event_id\":%lu,\"fire_at_utc\":%lld,\"fire_at_berlin\":\"%s\"}",
      id, state, error, static_cast<long long>(time(nullptr)), scheduleState,
      hasEvent ? static_cast<unsigned long>(schedule.id) : 0UL,
      hasEvent ? static_cast<long long>(schedule.fireAtUtc) : 0LL, berlinTime);
  if (length > 0 && length < static_cast<int>(sizeof(status.payload))) {
    if (xQueueSend(outbox, &status, 0) != pdTRUE) {
      Status discarded{};
      xQueueReceive(outbox, &discarded, 0);
      xQueueSend(outbox, &status, 0);
    }
  }
}

}  // namespace music_box
