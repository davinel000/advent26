#include "schedule.h"

#include <chrono>

namespace music_box {
namespace {

constexpr int64_t kGraceSeconds = 120;

int readDigits(std::string_view text, size_t offset, size_t count) {
  int value = 0;
  for (char digit : text.substr(offset, count)) {
    if (digit < '0' || digit > '9') {
      return -1;
    }
    value = value * 10 + digit - '0';
  }
  return value;
}

}  // namespace

bool validSchedule(const Schedule& schedule) {
  if (schedule.state == ScheduleState::empty) {
    return schedule.fireAtUtc == 0;
  }
  return schedule.id != 0 && schedule.fireAtUtc > 0 &&
         (schedule.state == ScheduleState::pending || schedule.state == ScheduleState::consumed ||
          schedule.state == ScheduleState::missed);
}

bool parseTimestamp(std::string_view text, int64_t& utc) {
  const bool withOffset = text.size() == 25;
  if (!withOffset && (text.size() != 20 || text[19] != 'Z')) {
    return false;
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
    return false;
  }

  const int year = readDigits(text, 0, 4);
  const int month = readDigits(text, 5, 2);
  const int day = readDigits(text, 8, 2);
  const int hour = readDigits(text, 11, 2);
  const int minute = readDigits(text, 14, 2);
  const int second = readDigits(text, 17, 2);
  using namespace std::chrono;
  const year_month_day date{
      std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
      std::chrono::day{static_cast<unsigned>(day)}};
  if (year < 1970 || !date.ok() || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59) {
    return false;
  }

  int offsetMinutes = 0;
  if (withOffset) {
    const char sign = text[19];
    const int offsetHour = readDigits(text, 20, 2);
    const int offsetMinute = readDigits(text, 23, 2);
    if ((sign != '+' && sign != '-') || text[22] != ':' || offsetHour < 0 || offsetHour > 14 ||
        offsetMinute < 0 || offsetMinute > 59 || (offsetHour == 14 && offsetMinute != 0)) {
      return false;
    }
    offsetMinutes = (offsetHour * 60 + offsetMinute) * (sign == '-' ? -1 : 1);
  }

  const auto local = sys_days{date} + hours{hour} + minutes{minute} + seconds{second};
  utc = (local - minutes{offsetMinutes}).time_since_epoch().count();
  return true;
}

DueAction dueAction(const Schedule& schedule, bool clockSynced, int64_t nowUtc) {
  if (!clockSynced || schedule.state != ScheduleState::pending || nowUtc < schedule.fireAtUtc) {
    return DueAction::wait;
  }
  return nowUtc - schedule.fireAtUtc <= kGraceSeconds ? DueAction::play : DueAction::miss;
}

unsigned adventDayDue(const std::tm& berlin, unsigned lastDay) {
  const auto day = static_cast<unsigned>(berlin.tm_mday);
  if (berlin.tm_year != 2026 - 1900 || berlin.tm_mon != 11 || day < 1 || day > 24 ||
      day <= lastDay) {
    return 0;
  }
  constexpr int start = (8 * 60 + 45) * 60;
  const int localSeconds = (berlin.tm_hour * 60 + berlin.tm_min) * 60 + berlin.tm_sec;
  return localSeconds >= start && localSeconds - start <= kGraceSeconds ? day : 0;
}

}  // namespace music_box
