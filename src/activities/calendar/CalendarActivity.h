#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

struct CalendarNote {
  int day;
  std::string text;
};

// Offline month-view calendar. Navigable with Left/Right (month) and
// Up/Down (day cursor within the month); Confirm opens a short text note for
// the selected day, stored on SD one file per month.
class CalendarActivity final : public Activity {
 private:
  int viewYear = 0;
  int viewMonth = 0;  // 1-12
  int selectedDay = 1;
  int todayYear = 0;
  int todayMonth = 0;
  int todayDay = 0;
  bool dateUnconfirmed = false;  // true if the clock looks unsynced (e.g. still at the 1970 epoch)

  std::vector<CalendarNote> notes;  // for the currently viewed month only

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

 public:
  explicit CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Calendar", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
