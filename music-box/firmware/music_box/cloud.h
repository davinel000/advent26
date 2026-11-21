#pragma once

#include "audio.h"
#include "schedule.h"

namespace music_box {

enum class Operation : uint8_t { play, stop, schedule, clear, status, count };
constexpr size_t kCommandIdSize = 32;

struct CloudCommand {
  char id[kCommandIdSize];
  Operation op;
  int64_t issuedAt;
  int64_t expiresAt;
  int64_t fireAtUtc;
  uint8_t melodyDay;
  Effect effect;
};

void startCloud();
void pollCloud(bool canConnect);
unsigned cloudSession();
bool nextCloudCommand(CloudCommand& command, const char*& error);
void reportCloud(const char* id, const char* state, const char* error,
                 const Schedule& schedule, bool storageReady);

}  // namespace music_box
