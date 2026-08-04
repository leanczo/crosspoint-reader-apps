#include "ReminderEditActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "ReminderDatePickerActivity.h"
#include "RemindersStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Editable fields: Date & Repeat, Description.
// Existing reminders also show a Delete option (BASE_ITEMS + 1).
constexpr int BASE_ITEMS = 2;
constexpr size_t MAX_DESCRIPTION_LENGTH = 80;

const char* repeatLabelFor(DateMath::RepeatRule repeat) {
  switch (repeat) {
    case DateMath::RepeatRule::Weekly:
      return tr(STR_REPEAT_WEEKLY);
    case DateMath::RepeatRule::Monthly:
      return tr(STR_REPEAT_MONTHLY);
    case DateMath::RepeatRule::Yearly:
      return tr(STR_REPEAT_YEARLY);
    case DateMath::RepeatRule::Never:
    default:
      return tr(STR_REPEAT_NEVER);
  }
}
}  // namespace

int ReminderEditActivity::getMenuItemCount() const { return isNewReminder ? BASE_ITEMS : BASE_ITEMS + 1; }

void ReminderEditActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  showSaveError = false;
  isNewReminder = (reminderIndex < 0);

  if (!isNewReminder) {
    const auto* reminder = REMINDERS_STORE.getReminder(static_cast<size_t>(reminderIndex));
    if (reminder) {
      editReminder = *reminder;
    } else {
      // Reminder was deleted between navigation and entering this screen — treat as new
      isNewReminder = true;
      reminderIndex = -1;
    }
  }

  if (isNewReminder) {
    editReminder = Reminder{};
    editReminder.year = today.year;
    editReminder.month = today.month;
    editReminder.day = today.day;
    editReminder.repeat = DateMath::RepeatRule::Never;
  }

  requestUpdate();
}

void ReminderEditActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int menuItems = getMenuItemCount();
  buttonNavigator.onNext([this, menuItems] {
    selectedIndex = (selectedIndex + 1) % menuItems;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuItems] {
    selectedIndex = (selectedIndex + menuItems - 1) % menuItems;
    requestUpdate();
  });
}

bool ReminderEditActivity::saveReminder() {
  bool success = false;

  if (isNewReminder) {
    // Create flow: first save inserts a new reminder record into the store.
    success = REMINDERS_STORE.addReminder(editReminder);
    if (success) {
      // After the first successful save, promote to an existing reminder so
      // subsequent field edits update in-place rather than creating duplicates.
      isNewReminder = false;
      reminderIndex = static_cast<int>(REMINDERS_STORE.getCount()) - 1;
    } else {
      LOG_ERR("RMS", "Failed to add reminder");
    }
  } else {
    // Edit flow: update the same reminder entry in-place.
    success = REMINDERS_STORE.updateReminder(static_cast<size_t>(reminderIndex), editReminder);
    if (!success) {
      LOG_ERR("RMS", "Failed to update reminder at index %d", reminderIndex);
    }
  }

  showSaveError = !success;
  if (showSaveError) {
    requestUpdate();
  }

  return success;
}

std::string ReminderEditActivity::formatDateRepeatValue() const {
  char buf[40];
  snprintf(buf, sizeof(buf), "%02d/%02d/%04d - %s", editReminder.day, editReminder.month, editReminder.year,
           repeatLabelFor(editReminder.repeat));
  return buf;
}

void ReminderEditActivity::handleSelection() {
  // Each field edit is saved immediately so partially configured reminders
  // survive navigation and power-loss scenarios.
  if (selectedIndex == 0) {
    // Date & Repeat
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& dateResult = std::get<ReminderDateResult>(result.data);
        editReminder.year = dateResult.year;
        editReminder.month = dateResult.month;
        editReminder.day = dateResult.day;
        editReminder.repeat = dateResult.repeat;
        saveReminder();
        requestUpdate();
      }
    };
    startActivityForResult(
        std::make_unique<ReminderDatePickerActivity>(renderer, mappedInput, editReminder.year, editReminder.month,
                                                      editReminder.day, editReminder.repeat),
        handler);
  } else if (selectedIndex == 1) {
    // Description
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editReminder.description = kb.text;
        saveReminder();
        requestUpdate();
      }
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_REMINDER_DESCRIPTION),
                                                 editReminder.description, MAX_DESCRIPTION_LENGTH, InputType::Text),
        handler);
  } else if (selectedIndex == 2 && !isNewReminder) {
    // Delete flow is only available for existing reminders, guarded by a
    // confirmation dialog since a reminder is user-authored content with no
    // external source to recover it from.
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        if (!REMINDERS_STORE.removeReminder(static_cast<size_t>(reminderIndex))) {
          LOG_ERR("RMS", "Failed to remove reminder at index %d", reminderIndex);
          showSaveError = true;
          requestUpdate();
          return;
        }
        finish();
      }
    };
    const std::string body =
        editReminder.description.empty() ? std::string(tr(STR_REMINDER_DESCRIPTION)) : editReminder.description;
    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), body),
                           handler);
  }
}

void ReminderEditActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const char* header = isNewReminder ? tr(STR_ADD_REMINDER) : tr(STR_REMINDERS_TITLE);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int menuItems = getMenuItemCount();

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, menuItems, static_cast<int>(selectedIndex),
      [](int index) {
        if (index == 0) return std::string(tr(STR_REMINDER_DATE_REPEAT));
        if (index == 1) return std::string(tr(STR_REMINDER_DESCRIPTION));
        return std::string(tr(STR_DELETE));
      },
      nullptr, nullptr,
      [this](int index) {
        if (index == 0) return formatDateRepeatValue();
        if (index == 1) {
          return editReminder.description.empty() ? std::string(tr(STR_NOT_SET)) : editReminder.description;
        }
        return std::string("");
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, ButtonArrow::Up,
                      ButtonArrow::Down);

  if (showSaveError) {
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  }

  renderer.displayBuffer();
}
