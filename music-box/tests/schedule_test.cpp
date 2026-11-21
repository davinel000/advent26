#include "schedule.h"

#include <cassert>

using music_box::DueAction;
using music_box::Schedule;
using music_box::ScheduleState;
using music_box::dueAction;
using music_box::parseTimestamp;

int main() {
  int64_t utc = -1;
  assert(parseTimestamp("1970-01-01T00:00:00Z", utc) && utc == 0);
  assert(parseTimestamp("1970-01-01T01:00:00+01:00", utc) && utc == 0);
  assert(parseTimestamp("1970-01-01T00:00:00-00:30", utc) && utc == 1800);
  assert(parseTimestamp("2040-02-29T23:59:59Z", utc) && utc > INT32_MAX);
  assert(!parseTimestamp("2026-02-29T12:00:00Z", utc));
  assert(!parseTimestamp("2026-12-01T24:00:00Z", utc));
  assert(!parseTimestamp("2026-12-01T12:00:00", utc));
  assert(!parseTimestamp("2026-12-01T12:00:00+14:01", utc));
  assert(!parseTimestamp("2026-12-01T12:00:nnZ", utc));

  int64_t summer = 0;
  int64_t winter = 0;
  assert(parseTimestamp("2026-10-25T02:30:00+02:00", summer));
  assert(parseTimestamp("2026-10-25T02:30:00+01:00", winter));
  assert(winter - summer == 3600);
  assert(parseTimestamp("2026-10-25T01:30:00Z", utc) && winter == utc);

  const Schedule pending{winter, 1, ScheduleState::pending};
  assert(music_box::validSchedule(pending));
  assert(music_box::validSchedule({0, 1, ScheduleState::empty}));
  assert(!music_box::validSchedule({0, 1, ScheduleState::pending}));
  assert(!music_box::validSchedule({winter, 0, ScheduleState::pending}));
  assert(!music_box::validSchedule({winter, 1, static_cast<ScheduleState>(7)}));

  // Start unsynced with a previously stored event, then simulate sync and time progression.
  assert(dueAction(pending, false, winter) == DueAction::wait);
  assert(dueAction(pending, true, winter - 1) == DueAction::wait);
  assert(dueAction(pending, true, winter) == DueAction::play);
  assert(dueAction(pending, true, winter + 120) == DueAction::play);
  assert(dueAction(pending, true, winter + 121) == DueAction::miss);

  // The consumed record persists across reboot and UTC corrections in either direction.
  const Schedule consumed{winter, 1, ScheduleState::consumed};
  assert(dueAction(consumed, false, winter) == DueAction::wait);
  assert(dueAction(consumed, true, winter - 3600) == DueAction::wait);
  assert(dueAction(consumed, true, winter) == DueAction::wait);
  assert(dueAction(consumed, true, winter + 3600) == DueAction::wait);
  assert(dueAction({winter, 1, ScheduleState::missed}, true, winter) == DueAction::wait);
  assert(dueAction({0, 1, ScheduleState::empty}, true, winter) == DueAction::wait);
}
