#include "DateMath.h"

namespace DateMath {

namespace {

// Floor division (rounds toward negative infinity), unlike C++'s truncating
// integer division. Needed so addMonths() wraps correctly for negative
// month deltas that cross a year boundary.
int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  int64_t r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) q--;
  return q;
}

}  // namespace

bool isLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

int daysInMonth(int year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) return isLeapYear(year) ? 29 : 28;
  return kDays[month - 1];
}

int clampDay(int year, int month, int day) {
  const int maxDay = daysInMonth(year, month);
  if (day < 1) return 1;
  if (day > maxDay) return maxDay;
  return day;
}

// Howard Hinnant's days_from_civil (public domain), adapted for CivilDate.
int64_t daysSinceEpoch(const CivilDate& d) {
  int64_t y = d.year;
  const unsigned m = d.month;
  const unsigned day = d.day;
  y -= (m <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);          // [0, 399]
  const unsigned doy = (153 * (m + (m > 2 ? -3u : 9u)) + 2) / 5 + day - 1;  // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;         // [0, 146096]
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Howard Hinnant's civil_from_days (public domain), adapted for CivilDate.
CivilDate civilFromDays(int64_t days) {
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);                 // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;    // [0, 399]
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                 // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                      // [0, 11]
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;                            // [1, 31]
  const unsigned month = mp + (mp < 10 ? 3 : -9);                               // [1, 12]

  CivilDate result;
  result.year = static_cast<int16_t>(y + (month <= 2));
  result.month = static_cast<uint8_t>(month);
  result.day = static_cast<uint8_t>(day);
  return result;
}

bool isBefore(const CivilDate& a, const CivilDate& b) { return daysSinceEpoch(a) < daysSinceEpoch(b); }

int64_t daysBetween(const CivilDate& from, const CivilDate& to) { return daysSinceEpoch(to) - daysSinceEpoch(from); }

CivilDate addMonths(const CivilDate& d, int months) {
  const int64_t totalMonths = static_cast<int64_t>(d.year) * 12 + (d.month - 1) + months;
  const int64_t newYear = floorDiv(totalMonths, 12);
  const int newMonth = static_cast<int>(totalMonths - newYear * 12) + 1;  // remainder in [0, 11] -> [1, 12]

  CivilDate result;
  result.year = static_cast<int16_t>(newYear);
  result.month = static_cast<uint8_t>(newMonth);
  result.day = static_cast<uint8_t>(clampDay(static_cast<int>(newYear), newMonth, d.day));
  return result;
}

CivilDate nextOccurrence(const CivilDate& anchor, RepeatRule rule, const CivilDate& today) {
  switch (rule) {
    case RepeatRule::Never:
      return anchor;

    case RepeatRule::Weekly: {
      const int64_t anchorDays = daysSinceEpoch(anchor);
      const int64_t todayDays = daysSinceEpoch(today);
      const int64_t diff = todayDays - anchorDays;
      if (diff <= 0) return anchor;
      const int64_t weeks = (diff + 6) / 7;  // ceiling division
      return civilFromDays(anchorDays + weeks * 7);
    }

    case RepeatRule::Monthly: {
      const int anchorMonths = anchor.year * 12 + (anchor.month - 1);
      const int todayMonths = today.year * 12 + (today.month - 1);
      int n = todayMonths - anchorMonths;
      if (n < 0) n = 0;
      CivilDate candidate = addMonths(anchor, n);
      if (isBefore(candidate, today)) {
        candidate = addMonths(anchor, n + 1);
      }
      return candidate;
    }

    case RepeatRule::Yearly: {
      int n = today.year - anchor.year;
      if (n < 0) n = 0;
      auto yearlyCandidate = [&](int yearsAhead) {
        const int y = anchor.year + yearsAhead;
        CivilDate c;
        c.year = static_cast<int16_t>(y);
        c.month = anchor.month;
        c.day = static_cast<uint8_t>(clampDay(y, anchor.month, anchor.day));
        return c;
      };
      CivilDate candidate = yearlyCandidate(n);
      if (isBefore(candidate, today)) {
        candidate = yearlyCandidate(n + 1);
      }
      return candidate;
    }
  }
  return anchor;
}

}  // namespace DateMath
