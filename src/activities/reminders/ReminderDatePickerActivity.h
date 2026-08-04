#pragma once

#include <DateMath.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Date + repeat-rule picker for a reminder. Four cyclable fields (Day, Month,
// Year, Repeat); Confirm cycles the active field, Up/Down adjust it.
// Mirrors ClockOffsetActivity's multi-field caret-cursor pattern.
class ReminderDatePickerActivity final : public Activity {
 public:
  explicit ReminderDatePickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int16_t year,
                                      uint8_t month, uint8_t day, DateMath::RepeatRule repeat)
      : Activity("ReminderDatePicker", renderer, mappedInput), year(year), month(month), day(day), repeat(repeat) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  enum Field { FIELD_DAY = 0, FIELD_MONTH = 1, FIELD_YEAR = 2, FIELD_REPEAT = 3, FIELD_COUNT };
  Field activeField = FIELD_DAY;

  int16_t year;
  uint8_t month;
  uint8_t day;
  DateMath::RepeatRule repeat;

  static constexpr int16_t MIN_YEAR = 1900;
  static constexpr int16_t MAX_YEAR = 2099;

  void adjustActiveField(int delta);
  void clampDayField();
};
