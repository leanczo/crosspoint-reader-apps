#pragma once

#include <cstdint>

// Pure calendar math: no Arduino/ESP32 dependencies, so this can be built and
// unit-tested on the host (see test/date_math). Only the Gregorian calendar
// is modeled; epoch day 0 is 1970-01-01 to match time_t day-numbering, even
// though nothing here touches time_t directly.
namespace DateMath {

enum class RepeatRule : uint8_t { Never = 0, Weekly = 1, Monthly = 2, Yearly = 3 };

struct CivilDate {
  int16_t year = 1970;
  uint8_t month = 1;  // 1-12
  uint8_t day = 1;    // 1-31
};

bool isLeapYear(int year);

// Number of days in `month` (1-12) of `year`.
int daysInMonth(int year, int month);

// Clamp `day` into [1, daysInMonth(year, month)].
int clampDay(int year, int month, int day);

// Days since the epoch (1970-01-01 = 0). Howard Hinnant's days_from_civil
// algorithm: closed-form, correct across the whole proleptic Gregorian range.
int64_t daysSinceEpoch(const CivilDate& d);

// Inverse of daysSinceEpoch (civil_from_days).
CivilDate civilFromDays(int64_t days);

bool isBefore(const CivilDate& a, const CivilDate& b);

// to - from, in days. Positive when `to` is in the future relative to `from`.
int64_t daysBetween(const CivilDate& from, const CivilDate& to);

// Add `months` (may be negative) to `d`, clamping the day into the resulting
// month's length. The clamp is always derived from `d.day`, never from an
// intermediate clamped value, so e.g. repeatedly advancing a day-31 date by
// one month yields 31, 28/29, 31, 30 (clamped), 31 (back to 31) - not a value
// stuck at 30 forever.
CivilDate addMonths(const CivilDate& d, int months);

// The first occurrence of a reminder anchored at `anchor` repeating per
// `rule` that is on or after `today`. For RepeatRule::Never this is always
// `anchor` unchanged. Closed-form (at most one "+1 correction"), never loops
// over periods, so it stays O(1) even for anchors far in the past.
CivilDate nextOccurrence(const CivilDate& anchor, RepeatRule rule, const CivilDate& today);

}  // namespace DateMath
