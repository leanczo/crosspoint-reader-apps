#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

struct CalendarNote {
  int day;
  std::string text;
};

struct Holiday {
  int month;  // 1-12
  int day;    // 1-31
  std::string name;
};

enum class CalendarState { Grid, HolidayList };

// Offline month-view calendar. Navigable with Left/Right (month) and
// Up/Down (day cursor within the month); Confirm opens a short text note for
// the selected day, stored on SD one file per month. Argentine public
// holidays (feriados) are an optional online overlay: fetched silently only
// when WiFi is already connected (never force-prompted, so the core grid
// stays fully usable offline), one call per year via api.argentinadatos.com.
class CalendarActivity final : public Activity {
 private:
  CalendarState state = CalendarState::Grid;

  int viewYear = 0;
  int viewMonth = 0;  // 1-12
  int selectedDay = 1;
  int todayYear = 0;
  int todayMonth = 0;
  int todayDay = 0;
  bool dateUnconfirmed = false;  // true if the clock looks unsynced (e.g. still at the 1970 epoch)

  std::vector<CalendarNote> notes;  // for the currently viewed month only

  // Feriados for `holidaysYear` (may lag `viewYear` right after a month/year
  // change until the load/fetch below catches up).
  std::vector<Holiday> holidays;
  int holidaysYear = 0;
  bool holidaysLoaded = false;
  bool holidaysRefreshing = false;
  bool holidaysRefreshFailed = false;
  std::string holidaysError;
  int holidayListSelectedRow = 0;

  volatile bool pendingUpdate = false;
  bool backgroundFetchSuccess = false;
  int fetchingYear = -1;
  void* fetchTaskHandle = nullptr;
  bool cancelFetch = false;

  static int daysInMonth(int year, int month);
  static int firstWeekdayOfMonth(int year, int month);  // 0=Monday..6=Sunday
  void getToday(int& year, int& month, int& day) const;

  std::string notesPath(int year, int month) const;
  void loadNotes();
  void saveNotes();
  std::string getNote(int day) const;
  void setNote(int day, const std::string& text);
  void goToMonth(int year, int month);
  void openNoteEditor();

  std::string holidaysCachePath(int year) const;
  std::string holidaysTmpPath(int year) const;
  std::string holidaysApiUrl(int year) const;
  static void appendDebugLog(const std::string& line);
  void ensureHolidaysForYear(int year);
  bool loadHolidaysFromSd(int year);
  void parseAndStoreHolidays(HalFile& file);
  void startHolidaysFetch(int year, bool promptWifi);
  void cancelHolidaysFetch();
  bool isHoliday(int month, int day, std::string* outName) const;

 public:
  void runBackgroundHolidaysFetch();  // called from the FreeRTOS task trampoline

  explicit CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Calendar", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
