#include "DateMath.h"

#include <gtest/gtest.h>

using DateMath::CivilDate;
using DateMath::RepeatRule;

namespace {

void ExpectDate(const CivilDate& actual, int year, int month, int day) {
  EXPECT_EQ(actual.year, year);
  EXPECT_EQ(actual.month, month);
  EXPECT_EQ(actual.day, day);
}

CivilDate MakeDate(int year, int month, int day) {
  CivilDate d;
  d.year = static_cast<int16_t>(year);
  d.month = static_cast<uint8_t>(month);
  d.day = static_cast<uint8_t>(day);
  return d;
}

}  // namespace

TEST(DateMath, IsLeapYear) {
  EXPECT_FALSE(DateMath::isLeapYear(1900));  // divisible by 100, not 400
  EXPECT_TRUE(DateMath::isLeapYear(2000));   // divisible by 400
  EXPECT_TRUE(DateMath::isLeapYear(2024));   // divisible by 4, not 100
  EXPECT_FALSE(DateMath::isLeapYear(2026));  // not divisible by 4
}

TEST(DateMath, DaysInMonthLeapYear) {
  EXPECT_EQ(DateMath::daysInMonth(2024, 2), 29);
  EXPECT_EQ(DateMath::daysInMonth(2025, 2), 28);
  EXPECT_EQ(DateMath::daysInMonth(1900, 2), 28);
  EXPECT_EQ(DateMath::daysInMonth(2000, 2), 29);
  EXPECT_EQ(DateMath::daysInMonth(2026, 4), 30);
  EXPECT_EQ(DateMath::daysInMonth(2026, 1), 31);
}

TEST(DateMath, EpochRoundTripKnownDates) {
  // 1970-01-01 is epoch day 0 by construction.
  EXPECT_EQ(DateMath::daysSinceEpoch(MakeDate(1970, 1, 1)), 0);
  // 2000-01-01 is a well-known reference: 10957 days after the epoch.
  EXPECT_EQ(DateMath::daysSinceEpoch(MakeDate(2000, 1, 1)), 10957);

  ExpectDate(DateMath::civilFromDays(0), 1970, 1, 1);
  ExpectDate(DateMath::civilFromDays(10957), 2000, 1, 1);
}

TEST(DateMath, EpochRoundTripSweep) {
  // Sweep every day across a range spanning several leap-year cycles and
  // confirm daysSinceEpoch/civilFromDays are exact inverses.
  for (int year = 1900; year <= 2100; year++) {
    for (int month = 1; month <= 12; month++) {
      const int total = DateMath::daysInMonth(year, month);
      for (int day = 1; day <= total; day += 7) {  // sample every 7th day, full coverage would be slow
        const CivilDate original = MakeDate(year, month, day);
        const int64_t days = DateMath::daysSinceEpoch(original);
        const CivilDate roundTripped = DateMath::civilFromDays(days);
        ASSERT_EQ(roundTripped.year, original.year) << year << "-" << month << "-" << day;
        ASSERT_EQ(roundTripped.month, original.month) << year << "-" << month << "-" << day;
        ASSERT_EQ(roundTripped.day, original.day) << year << "-" << month << "-" << day;
      }
    }
  }
}

TEST(DateMath, ClampDay) {
  EXPECT_EQ(DateMath::clampDay(2026, 4, 31), 30);  // April has 30 days
  EXPECT_EQ(DateMath::clampDay(2026, 2, 31), 28);  // Feb 2026 is not a leap year
  EXPECT_EQ(DateMath::clampDay(2024, 2, 29), 29);  // Feb 2024 is a leap year
  EXPECT_EQ(DateMath::clampDay(2026, 1, 15), 15);  // within range, unchanged
}

TEST(DateMath, AddMonthsClampsFromOriginalAnchorDayEachTime) {
  // Anchor day 31. Each call must clamp from the anchor's own day (31), not
  // from a previously-clamped intermediate result, so April's clamp to 30
  // doesn't "stick" - May must snap back to 31.
  const CivilDate anchor = MakeDate(2026, 1, 31);
  ExpectDate(DateMath::addMonths(anchor, 0), 2026, 1, 31);
  ExpectDate(DateMath::addMonths(anchor, 1), 2026, 2, 28);  // Feb 2026, not a leap year
  ExpectDate(DateMath::addMonths(anchor, 2), 2026, 3, 31);
  ExpectDate(DateMath::addMonths(anchor, 3), 2026, 4, 30);  // clamped
  ExpectDate(DateMath::addMonths(anchor, 4), 2026, 5, 31);  // snaps back to 31
}

TEST(DateMath, AddMonthsNegativeCrossesYearBoundary) {
  ExpectDate(DateMath::addMonths(MakeDate(2026, 1, 15), -1), 2025, 12, 15);
  ExpectDate(DateMath::addMonths(MakeDate(2026, 3, 31), -4), 2025, 11, 30);  // clamped
}

TEST(DateMath, NextOccurrenceNeverAlwaysReturnsAnchor) {
  const CivilDate anchor = MakeDate(2020, 6, 15);
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Never, MakeDate(2020, 6, 14)), 2020, 6, 15);
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Never, MakeDate(2020, 6, 15)), 2020, 6, 15);
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Never, MakeDate(2030, 1, 1)), 2020, 6, 15);
}

TEST(DateMath, NextOccurrenceWeeklyCeilingEdgeCases) {
  const CivilDate anchor = MakeDate(2026, 1, 1);
  // today == anchor: the anchor itself is the next occurrence.
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Weekly, MakeDate(2026, 1, 1)), 2026, 1, 1);
  // one day before the +7 occurrence: still rolls forward to it.
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Weekly, MakeDate(2026, 1, 7)), 2026, 1, 8);
  // exactly on the +7 occurrence.
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Weekly, MakeDate(2026, 1, 8)), 2026, 1, 8);
  // one day after: rolls forward to +14.
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Weekly, MakeDate(2026, 1, 9)), 2026, 1, 15);
}

TEST(DateMath, NextOccurrenceMonthlyClampAndSnapBack) {
  const CivilDate anchor = MakeDate(2026, 1, 31);
  // Before April's occurrence has passed: it's still the next one (clamped to 30).
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Monthly, MakeDate(2026, 4, 15)), 2026, 4, 30);
  // Exactly on April's clamped occurrence.
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Monthly, MakeDate(2026, 4, 30)), 2026, 4, 30);
  // One day after April's occurrence passed: rolls to May, which snaps back to 31.
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Monthly, MakeDate(2026, 5, 1)), 2026, 5, 31);
}

TEST(DateMath, NextOccurrenceYearlyFeb29ClampsAndReverts) {
  const CivilDate anchor = MakeDate(2024, 2, 29);  // leap year anchor
  // 2025 is not a leap year: clamps to Feb 28.
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Yearly, MakeDate(2025, 1, 1)), 2025, 2, 28);
  // After Feb 2026 (also not a leap year) has passed, rolls to 2027, still clamped.
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Yearly, MakeDate(2026, 3, 1)), 2027, 2, 28);
  // 2028 is a leap year: the anchor's day 29 is recomputed fresh, so it reverts to Feb 29.
  ExpectDate(DateMath::nextOccurrence(anchor, RepeatRule::Yearly, MakeDate(2028, 1, 1)), 2028, 2, 29);
}

TEST(DateMath, DaysBetweenSignConvention) {
  EXPECT_EQ(DateMath::daysBetween(MakeDate(2026, 1, 1), MakeDate(2026, 1, 2)), 1);   // future -> positive
  EXPECT_EQ(DateMath::daysBetween(MakeDate(2026, 1, 2), MakeDate(2026, 1, 1)), -1);  // past -> negative
  EXPECT_EQ(DateMath::daysBetween(MakeDate(2026, 1, 1), MakeDate(2026, 1, 1)), 0);
}
