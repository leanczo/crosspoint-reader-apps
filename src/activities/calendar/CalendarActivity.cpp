#include "CalendarActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* MONTH_NAMES[12] = {"January", "February", "March",     "April",   "May",      "June",
                                         "July",    "August",   "September", "October", "November", "December"};
constexpr const char* WEEKDAY_ABBR[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
}  // namespace

int CalendarActivity::daysInMonth(int year, int month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return days[month - 1];
}

// Sakamoto's algorithm: day of week for a given date, 0=Sunday..6=Saturday.
// Computed directly instead of via mktime/localtime_r so it doesn't depend on
// libc's (unconfigured) timezone state.
int CalendarActivity::firstWeekdayOfMonth(int year, int month) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  if (month < 3) y -= 1;
  const int day = 1;
  const int wday = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
  return (wday + 6) % 7;  // remap 0=Sunday..6=Saturday to 0=Monday..6=Sunday
}

void CalendarActivity::getToday(int& year, int& month, int& day) const {
  const time_t now = time(nullptr);
  // Same manual UTC-offset convention as ClockActivity, applied to the raw
  // epoch before splitting into calendar fields so a date rollover near
  // midnight is handled correctly without relying on libc timezone support.
  const int offsetMinutes = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  const time_t adjusted = now + static_cast<time_t>(offsetMinutes) * 60;
  struct tm t;
  gmtime_r(&adjusted, &t);
  year = t.tm_year + 1900;
  month = t.tm_mon + 1;
  day = t.tm_mday;
}

std::string CalendarActivity::notesPath(int year, int month) const {
  char buf[40];
  snprintf(buf, sizeof(buf), "/apps/calendar/%04d-%02d.txt", year, month);
  return buf;
}

void CalendarActivity::loadNotes() {
  notes.clear();
  String content = Storage.readFile(notesPath(viewYear, viewMonth).c_str());
  std::string data(content.c_str());

  size_t pos = 0;
  while (pos < data.length()) {
    size_t nl = data.find('\n', pos);
    std::string line = (nl == std::string::npos) ? data.substr(pos) : data.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t sep = line.find('|');
    if (!line.empty() && sep != std::string::npos) {
      const int day = atoi(line.substr(0, sep).c_str());
      if (day >= 1 && day <= 31) {
        notes.push_back(CalendarNote{day, line.substr(sep + 1)});
      }
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
}

void CalendarActivity::saveNotes() {
  String content;
  for (const auto& note : notes) {
    content += String(note.day) + "|" + String(note.text.c_str()) + "\n";
  }
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/calendar");
  Storage.writeFile(notesPath(viewYear, viewMonth).c_str(), content);
}

std::string CalendarActivity::getNote(int day) const {
  for (const auto& note : notes) {
    if (note.day == day) return note.text;
  }
  return "";
}

void CalendarActivity::setNote(int day, const std::string& text) {
  for (auto it = notes.begin(); it != notes.end(); ++it) {
    if (it->day == day) {
      if (text.empty()) {
        notes.erase(it);
      } else {
        it->text = text;
      }
      return;
    }
  }
  if (!text.empty()) {
    notes.push_back(CalendarNote{day, text});
  }
}

void CalendarActivity::goToMonth(int year, int month) {
  viewYear = year;
  viewMonth = month;
  const int total = daysInMonth(viewYear, viewMonth);
  if (viewYear == todayYear && viewMonth == todayMonth) {
    selectedDay = todayDay;
  } else {
    selectedDay = std::min(selectedDay, total);
  }
  loadNotes();
  requestUpdate();
}

void CalendarActivity::openNoteEditor() {
  char titleBuf[48];
  snprintf(titleBuf, sizeof(titleBuf), "Note: %s %d, %d", MONTH_NAMES[viewMonth - 1], selectedDay, viewYear);
  const std::string existing = getNote(selectedDay);
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, titleBuf, existing, 200);
  const int day = selectedDay;
  startActivityForResult(std::move(keyboard), [this, day](const ActivityResult& result) {
    if (!result.isCancelled) {
      auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
      if (keyboardResult) {
        setNote(day, keyboardResult->text);
        saveNotes();
      }
    }
    requestUpdate();
  });
}

void CalendarActivity::onEnter() {
  Activity::onEnter();
  getToday(todayYear, todayMonth, todayDay);

  // Fresh boot with no WiFi sync yet leaves the system clock at the 1970
  // epoch, which showed up as "today" being wrong. Only fix this silently
  // (no connect prompt - this app should still work fully offline) when WiFi
  // is already associated; otherwise just flag it instead of pretending the
  // epoch date is correct.
  if (todayYear < 2024) {
    if (WiFi.status() == WL_CONNECTED && WifiConnectHelper::waitForTimeSync()) {
      getToday(todayYear, todayMonth, todayDay);
    }
    dateUnconfirmed = todayYear < 2024;
  } else {
    dateUnconfirmed = false;
  }

  viewYear = todayYear;
  viewMonth = todayMonth;
  selectedDay = todayDay;
  loadNotes();
  requestUpdate();
}

void CalendarActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int total = daysInMonth(viewYear, viewMonth);

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    selectedDay = selectedDay > 1 ? selectedDay - 1 : total;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selectedDay = selectedDay < total ? selectedDay + 1 : 1;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    int y = viewYear;
    int m = viewMonth - 1;
    if (m < 1) {
      m = 12;
      y--;
    }
    goToMonth(y, m);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    int y = viewYear;
    int m = viewMonth + 1;
    if (m > 12) {
      m = 1;
      y++;
    }
    goToMonth(y, m);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openNoteEditor();
  }
}

void CalendarActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CALENDAR_TITLE));

  char subBuf[24];
  snprintf(subBuf, sizeof(subBuf), "%s %d", MONTH_NAMES[viewMonth - 1], viewYear);
  const int subHeaderY = metrics.topPadding + metrics.headerHeight;
  constexpr int SUBHEADER_H = 30;
  GUI.drawSubHeader(renderer, Rect{0, subHeaderY, pageWidth, SUBHEADER_H}, subBuf, nullptr);

  constexpr int GRID_MARGIN_X = 10;
  constexpr int WEEKDAY_HEADER_H = 22;
  constexpr int NOTE_AREA_H = 34;

  const int gridTop = subHeaderY + SUBHEADER_H + 6;
  const int gridBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - NOTE_AREA_H;

  const int gridW = pageWidth - 2 * GRID_MARGIN_X;
  const int cellW = gridW / 7;
  const int daysGridTop = gridTop + WEEKDAY_HEADER_H;
  const int cellH = (gridBottom - daysGridTop) / 6;

  for (int i = 0; i < 7; i++) {
    const int x = GRID_MARGIN_X + i * cellW;
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, WEEKDAY_ABBR[i]);
    renderer.drawText(SMALL_FONT_ID, x + (cellW - tw) / 2, gridTop, WEEKDAY_ABBR[i], true);
  }

  const int firstWd = firstWeekdayOfMonth(viewYear, viewMonth);
  const int total = daysInMonth(viewYear, viewMonth);

  for (int day = 1; day <= total; day++) {
    const int cellIndex = firstWd + (day - 1);
    const int row = cellIndex / 7;
    const int col = cellIndex % 7;
    const int cx = GRID_MARGIN_X + col * cellW;
    const int cy = daysGridTop + row * cellH;

    const bool isToday = (viewYear == todayYear && viewMonth == todayMonth && day == todayDay);
    const bool isSelected = (day == selectedDay);
    const bool hasNote = !getNote(day).empty();

    if (isSelected) {
      renderer.fillRect(cx + 1, cy + 1, cellW - 2, cellH - 2, true);
    } else if (isToday) {
      renderer.drawRect(cx + 1, cy + 1, cellW - 2, cellH - 2, 2, true);
    }

    char dayBuf[3];
    snprintf(dayBuf, sizeof(dayBuf), "%d", day);
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, dayBuf);
    const int th = renderer.getLineHeight(UI_12_FONT_ID);
    const int tx = cx + (cellW - tw) / 2;
    const int ty = cy + (cellH - th) / 2;
    renderer.drawText(UI_12_FONT_ID, tx, ty, dayBuf, !isSelected);

    if (hasNote) {
      const int dotY = cy + cellH - 7;
      renderer.fillRect(cx + cellW / 2 - 2, dotY, 4, 4, !isSelected);
    }
  }

  const int noteY = gridBottom + 6;
  if (dateUnconfirmed) {
    renderer.drawText(SMALL_FONT_ID, GRID_MARGIN_X, noteY, "Clock not synced - connect WiFi to fix today's date",
                      true);
  } else {
    const std::string note = getNote(selectedDay);
    if (!note.empty()) {
      std::string preview = note;
      constexpr size_t MAX_PREVIEW = 70;
      if (preview.length() > MAX_PREVIEW) {
        preview = preview.substr(0, MAX_PREVIEW - 3) + "...";
      }
      renderer.drawText(SMALL_FONT_ID, GRID_MARGIN_X, noteY, preview.c_str(), true);
    }
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_CALENDAR_NOTE), tr(STR_PREVIOUS_TAB), tr(STR_NEXT_TAB));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
