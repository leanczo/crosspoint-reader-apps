#include "RemindersListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReminderEditActivity.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"

DateMath::CivilDate RemindersListActivity::getToday() const {
  const time_t now = time(nullptr);
  // Same manual UTC-offset convention as CalendarActivity::getToday(),
  // applied to the raw epoch before splitting into calendar fields so a date
  // rollover near midnight is handled correctly without relying on libc
  // timezone support (unconfigured on this firmware).
  const int offsetMinutes = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  const time_t adjusted = now + static_cast<time_t>(offsetMinutes) * 60;
  struct tm t;
  gmtime_r(&adjusted, &t);
  DateMath::CivilDate result;
  result.year = static_cast<int16_t>(t.tm_year + 1900);
  result.month = static_cast<uint8_t>(t.tm_mon + 1);
  result.day = static_cast<uint8_t>(t.tm_mday);
  return result;
}

DateMath::CivilDate RemindersListActivity::effectiveDateOf(const Reminder& reminder) const {
  DateMath::CivilDate anchor;
  anchor.year = reminder.year;
  anchor.month = reminder.month;
  anchor.day = reminder.day;
  if (reminder.repeat == DateMath::RepeatRule::Never) return anchor;
  return DateMath::nextOccurrence(anchor, reminder.repeat, today);
}

void RemindersListActivity::rebuildSortOrder() {
  const auto& reminders = REMINDERS_STORE.getReminders();
  sortOrder.clear();
  sortOrder.reserve(reminders.size());
  for (size_t i = 0; i < reminders.size(); i++) sortOrder.push_back(i);

  std::stable_sort(sortOrder.begin(), sortOrder.end(), [this, &reminders](size_t a, size_t b) {
    return DateMath::daysSinceEpoch(effectiveDateOf(reminders[a])) <
           DateMath::daysSinceEpoch(effectiveDateOf(reminders[b]));
  });
}

int RemindersListActivity::getItemCount() const { return static_cast<int>(sortOrder.size()) + 1; }

void RemindersListActivity::onEnter() {
  Activity::onEnter();

  today = getToday();

  // Fresh boot with no WiFi sync yet leaves the system clock at the 1970
  // epoch, which would show every reminder as ~55 years overdue. Only fix
  // this silently (no connect prompt - this app should still work fully
  // offline) when WiFi is already associated; otherwise just flag it
  // instead of pretending the epoch date is correct.
  if (today.year < 2024) {
    if (WiFi.status() == WL_CONNECTED && WifiConnectHelper::waitForTimeSync()) {
      today = getToday();
    }
    dateUnconfirmed = today.year < 2024;
  } else {
    dateUnconfirmed = false;
  }

  REMINDERS_STORE.loadFromFile();
  rebuildSortOrder();
  selectedIndex = 0;
  requestUpdate();
}

void RemindersListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int itemCount = getItemCount();
  if (itemCount > 0) {
    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }
}

void RemindersListActivity::handleSelection() {
  const int reminderCount = static_cast<int>(sortOrder.size());
  const int storeIndex = (selectedIndex < reminderCount) ? static_cast<int>(sortOrder[selectedIndex]) : -1;

  auto handler = [this](const ActivityResult&) {
    // Reload from disk and re-sort in case the reminder was added/edited/deleted.
    // Resets selection to the top - the sort order may have changed, so there's
    // no stable position to restore (same simplification OpdsServerListActivity uses).
    REMINDERS_STORE.loadFromFile();
    rebuildSortOrder();
    selectedIndex = 0;
    requestUpdate();
  };

  startActivityForResult(std::make_unique<ReminderEditActivity>(renderer, mappedInput, storeIndex, today), handler);
}

std::string RemindersListActivity::formatRelativeDate(const DateMath::CivilDate& effective) const {
  const int64_t diff = DateMath::daysBetween(today, effective);
  if (diff == 0) {
    return tr(STR_REMINDER_TODAY);
  }

  char buf[24];
  if (diff > 0) {
    snprintf(buf, sizeof(buf), tr(STR_REMINDER_IN_DAYS), static_cast<int>(diff));
  } else {
    snprintf(buf, sizeof(buf), tr(STR_REMINDER_DAYS_AGO), static_cast<int>(-diff));
  }
  return buf;
}

void RemindersListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_REMINDERS_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int itemCount = getItemCount();
  const int reminderCount = static_cast<int>(sortOrder.size());
  const auto& reminders = REMINDERS_STORE.getReminders();

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
      [this, &reminders, reminderCount](int index) {
        if (index < reminderCount) {
          const auto& reminder = reminders[sortOrder[static_cast<size_t>(index)]];
          return reminder.description.empty() ? std::string(tr(STR_NOT_SET)) : reminder.description;
        }
        return std::string(tr(STR_ADD_REMINDER));
      },
      nullptr, nullptr,
      [this, &reminders, reminderCount](int index) {
        if (index < reminderCount) {
          return formatRelativeDate(effectiveDateOf(reminders[sortOrder[static_cast<size_t>(index)]]));
        }
        return std::string("");
      });

  if (dateUnconfirmed) {
    const int noteY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 16;
    renderer.drawText(SMALL_FONT_ID, 10, noteY, tr(STR_REMINDER_CLOCK_UNSYNCED), true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, ButtonArrow::Up,
                      ButtonArrow::Down);

  renderer.displayBuffer();
}
