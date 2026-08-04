#pragma once

#include <DateMath.h>

#include <string>
#include <vector>

#include "RemindersStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// List of user reminders, sorted chronologically by "effective date" (the
// next occurrence for repeating reminders, or the anchor date otherwise) —
// most overdue first, then today, then future. Confirm opens the selected
// reminder for editing, or a virtual last row to add a new one.
class RemindersListActivity final : public Activity {
 public:
  explicit RemindersListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RemindersList", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  std::vector<size_t> sortOrder;  // permutation of RemindersStore indices, sorted by effective date
  DateMath::CivilDate today{};
  bool dateUnconfirmed = false;

  DateMath::CivilDate getToday() const;
  DateMath::CivilDate effectiveDateOf(const Reminder& reminder) const;
  void rebuildSortOrder();
  int getItemCount() const;
  void handleSelection();
  std::string formatRelativeDate(const DateMath::CivilDate& effective) const;
};
