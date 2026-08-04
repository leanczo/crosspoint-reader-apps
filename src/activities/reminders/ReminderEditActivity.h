#pragma once

#include <DateMath.h>

#include <string>

#include "RemindersStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Edit screen for a single reminder.
 * Shows "Date & Repeat" and "Description" fields, plus a Delete option when
 * editing an existing reminder. Used for both adding new reminders and
 * editing existing ones.
 */
class ReminderEditActivity final : public Activity {
 public:
  /**
   * @param reminderIndex Index into RemindersStore, or -1 for a new reminder
   * @param today Used to default a new reminder's date; ignored when editing
   */
  explicit ReminderEditActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int reminderIndex,
                                DateMath::CivilDate today)
      : Activity("ReminderEdit", renderer, mappedInput), reminderIndex(reminderIndex), today(today) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;
  int reminderIndex;
  DateMath::CivilDate today;
  Reminder editReminder;
  bool isNewReminder = false;
  bool showSaveError = false;

  int getMenuItemCount() const;
  void handleSelection();
  bool saveReminder();
  std::string formatDateRepeatValue() const;
};
