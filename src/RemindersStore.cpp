#include "RemindersStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

RemindersStore RemindersStore::instance;

namespace {
constexpr char REMINDERS_FILE_JSON[] = "/apps/reminders/reminders.json";
}  // namespace

bool RemindersStore::saveToFile() const {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/reminders");
  return JsonSettingsIO::saveReminders(*this, REMINDERS_FILE_JSON);
}

bool RemindersStore::loadFromFile() {
  if (!Storage.exists(REMINDERS_FILE_JSON)) {
    reminders.clear();
    return false;
  }

  String json = Storage.readFile(REMINDERS_FILE_JSON);
  if (json.isEmpty()) {
    reminders.clear();
    return false;
  }

  return JsonSettingsIO::loadReminders(*this, json.c_str());
}

bool RemindersStore::addReminder(const Reminder& reminder) {
  if (reminders.size() >= MAX_REMINDERS) {
    LOG_DBG("RMS", "Cannot add more reminders, limit of %zu reached", MAX_REMINDERS);
    return false;
  }

  reminders.push_back(reminder);
  LOG_DBG("RMS", "Added reminder: %s", reminder.description.c_str());
  return saveToFile();
}

bool RemindersStore::updateReminder(size_t index, const Reminder& reminder) {
  if (index >= reminders.size()) {
    return false;
  }

  reminders[index] = reminder;
  LOG_DBG("RMS", "Updated reminder: %s", reminder.description.c_str());
  return saveToFile();
}

bool RemindersStore::removeReminder(size_t index) {
  if (index >= reminders.size()) {
    return false;
  }

  LOG_DBG("RMS", "Removed reminder: %s", reminders[index].description.c_str());
  reminders.erase(reminders.begin() + static_cast<ptrdiff_t>(index));
  return saveToFile();
}

const Reminder* RemindersStore::getReminder(size_t index) const {
  if (index >= reminders.size()) {
    return nullptr;
  }
  return &reminders[index];
}
