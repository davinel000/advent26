#pragma once

#include <cstdint>
#include <string_view>

namespace music_box {

enum class ScheduleState : uint32_t { empty, pending, consumed, missed };

// One NVS blob holds the timestamp, ID and state together.
struct Schedule {
  int64_t fireAtUtc;
  uint32_t id;
  ScheduleState state;
};

static_assert(sizeof(Schedule) == 16);

enum class DueAction { wait, play, miss };

bool validSchedule(const Schedule& schedule);
bool parseTimestamp(std::string_view text, int64_t& utc);
DueAction dueAction(const Schedule& schedule, bool clockSynced, int64_t nowUtc);

}  // namespace music_box
