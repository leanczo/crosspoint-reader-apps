#include "CalendarActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/DownloadWatchdog.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr const char* MONTH_NAMES[12] = {"January", "February", "March",     "April",   "May",      "June",
                                         "July",    "August",   "September", "October", "November", "December"};
constexpr const char* WEEKDAY_ABBR[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};

// Used only by the new Holiday List screen's date formatting -- unlike
// MONTH_NAMES above (pre-existing, used by the grid's own subheader and left
// untouched here), this goes through tr() per the project's i18n rule for
// any newly-written display code.
constexpr StrId kMonthKeys[12] = {
    StrId::STR_MONTH_JANUARY, StrId::STR_MONTH_FEBRUARY, StrId::STR_MONTH_MARCH,     StrId::STR_MONTH_APRIL,
    StrId::STR_MONTH_MAY,     StrId::STR_MONTH_JUNE,     StrId::STR_MONTH_JULY,      StrId::STR_MONTH_AUGUST,
    StrId::STR_MONTH_SEPTEMBER, StrId::STR_MONTH_OCTOBER, StrId::STR_MONTH_NOVEMBER, StrId::STR_MONTH_DECEMBER};

// Short per-attempt timeout for this small JSON fetch (paired with the 3x
// retry loop in runBackgroundHolidaysFetch()), matching the convention used
// by every other fetch-capable app in this codebase.
constexpr uint32_t kFetchTimeoutMs = 10000;

void calendarHolidaysFetchTaskFunc(void* param) {
  auto* activity = static_cast<CalendarActivity*>(param);
  activity->runBackgroundHolidaysFetch();
  vTaskDelete(nullptr);
}
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

std::string CalendarActivity::holidaysCachePath(int year) const {
  char buf[40];
  snprintf(buf, sizeof(buf), "/apps/calendar/feriados_%04d.json", year);
  return buf;
}

std::string CalendarActivity::holidaysTmpPath(int year) const { return holidaysCachePath(year) + ".tmp"; }

std::string CalendarActivity::holidaysApiUrl(int year) const {
  return "https://api.argentinadatos.com/v1/feriados/" + std::to_string(year);
}

void CalendarActivity::appendDebugLog(const std::string& line) {
  String existing = Storage.readFile("/apps/calendar/debug.log");
  String entry = existing + String(millis()) + "ms: " + String(line.c_str()) + "\n";
  constexpr size_t MAX_LOG_CHARS = 4000;
  if (entry.length() > MAX_LOG_CHARS) {
    entry = entry.substring(entry.length() - MAX_LOG_CHARS);
  }
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/calendar");
  Storage.writeFile("/apps/calendar/debug.log", entry);
}

bool CalendarActivity::loadHolidaysFromSd(int year) {
  HalFile file;
  if (!Storage.openFileForRead("CALENDAR", holidaysCachePath(year).c_str(), file)) {
    return false;
  }
  parseAndStoreHolidays(file);
  holidaysYear = year;
  return holidaysLoaded;
}

void CalendarActivity::parseAndStoreHolidays(HalFile& file) {
  struct HalFileJsonReader {
    HalFile& f;
    int read() { return f.read(); }
    size_t readBytes(char* buffer, size_t length) {
      const int n = f.read(buffer, length);
      return n < 0 ? 0 : static_cast<size_t>(n);
    }
  } reader{file};

  JsonDocument filter;
  filter[0]["fecha"] = true;
  filter[0]["nombre"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
  if (err) {
    appendDebugLog(std::string("JSON parse failed: ") + err.c_str());
    holidaysError = tr(STR_CALENDAR_HOLIDAYS_NO_DATA);
    return;
  }

  std::vector<Holiday> parsed;
  JsonArray items = doc.as<JsonArray>();
  parsed.reserve(items.size());
  for (JsonObject item : items) {
    std::string fecha = item["fecha"] | "";  // "YYYY-MM-DD"
    std::string nombre = item["nombre"] | "";
    if (fecha.length() != 10 || nombre.empty()) continue;
    Holiday h;
    h.month = atoi(fecha.substr(5, 2).c_str());
    h.day = atoi(fecha.substr(8, 2).c_str());
    h.name = std::move(nombre);
    parsed.push_back(std::move(h));
  }

  holidays = std::move(parsed);
  holidaysLoaded = !holidays.empty();
  if (holidaysLoaded) {
    holidaysError.clear();
  } else {
    holidaysError = tr(STR_CALENDAR_HOLIDAYS_NO_DATA);
  }
}

// Cooperative cancel + bounded wait, same pattern as every other fetch-capable
// app in this codebase (Football/HackerNews/OnThisDay).
void CalendarActivity::cancelHolidaysFetch() {
  if (fetchTaskHandle != nullptr) {
    holidaysRefreshing = false;
    cancelFetch = true;
    int waitCount = 0;
    while (fetchTaskHandle != nullptr && waitCount < 1200) {
      delay(10);
      waitCount++;
    }
    if (fetchTaskHandle != nullptr) {
      LOG_ERR("CALENDAR", "Task failed to exit gracefully, forcing vTaskDelete!");
      appendDebugLog("Task failed to exit gracefully, forcing vTaskDelete!");
      TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
      fetchTaskHandle = nullptr;
      vTaskDelete(tempHandle);
    }
  }
  pendingUpdate = false;
}

// promptWifi=false is used for the silent "cache is empty but WiFi already
// happens to be connected" auto-load; promptWifi=true is used only by the
// Holiday List screen's explicit Refresh row, matching every other app's
// manual-refresh convention (see the "Fetch policy" note in the class-level
// doc comment for why this app is more conservative than Football/HN/OnThisDay).
void CalendarActivity::startHolidaysFetch(int year, bool promptWifi) {
  cancelHolidaysFetch();
  fetchingYear = year;
  cancelFetch = false;
  holidaysRefreshing = true;
  holidaysRefreshFailed = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/calendar");

  auto spawn = [this]() {
    backgroundFetchSuccess = false;
    xTaskCreate(calendarHolidaysFetchTaskFunc, "cal_holidays", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
  };
  auto onCancelledOrFailed = [this]() {
    appendDebugLog("WiFi connect cancelled/failed for year " + std::to_string(fetchingYear));
    holidaysRefreshing = false;
    loadHolidaysFromSd(fetchingYear);
    if (!holidaysLoaded) holidaysError = tr(STR_CALENDAR_HOLIDAYS_NO_DATA);
    requestUpdate();
  };

  if (promptWifi) {
    ensureWifiConnected(spawn, onCancelledOrFailed);
  } else if (WiFi.status() == WL_CONNECTED) {
    spawn();
  } else {
    holidaysRefreshing = false;
  }
  requestUpdate();
}

void CalendarActivity::runBackgroundHolidaysFetch() {
  DownloadWatchdog::start(30000);
  WifiConnectHelper::waitForTimeSync();

  bool success = false;
  int retries = 3;
  int attempt = 0;
  while (retries > 0 && !DownloadWatchdog::gotTimeout && !cancelFetch) {
    attempt++;
    std::string errDetail;
    const auto result = HttpDownloader::downloadToFile(holidaysApiUrl(fetchingYear), holidaysTmpPath(fetchingYear),
                                                        nullptr, &cancelFetch, "", "", nullptr, nullptr, &errDetail,
                                                        kFetchTimeoutMs);
    if (result == HttpDownloader::OK) {
      success = true;
      break;
    }
    retries--;
    appendDebugLog("Attempt " + std::to_string(attempt) + ": result=" + std::to_string(static_cast<int>(result)) +
                   " detail=" + errDetail + ", " + std::to_string(retries) + " retries left");
    if (retries > 0 && !DownloadWatchdog::gotTimeout && !cancelFetch) delay(1000);
  }

  DownloadWatchdog::stop();
  if (DownloadWatchdog::gotTimeout) success = false;

  if (cancelFetch) {
    fetchTaskHandle = nullptr;
    return;
  }

  fetchTaskHandle = nullptr;
  backgroundFetchSuccess = success;
  pendingUpdate = true;
}

// Called whenever the viewed year might have changed (onEnter, goToMonth).
// Loads from SD cache if present; otherwise, per the conservative fetch
// policy, only auto-fetches silently when WiFi happens to already be
// connected -- never force-prompts, so the core grid stays fully offline-first.
void CalendarActivity::ensureHolidaysForYear(int year) {
  if (year == holidaysYear && (holidaysLoaded || !holidaysError.empty())) return;
  holidays.clear();
  holidaysLoaded = false;
  holidaysError.clear();
  holidaysYear = year;
  if (!loadHolidaysFromSd(year)) {
    startHolidaysFetch(year, /*promptWifi=*/false);
  }
}

bool CalendarActivity::isHoliday(int month, int day, std::string* outName) const {
  for (const auto& h : holidays) {
    if (h.month == month && h.day == day) {
      if (outName) *outName = h.name;
      return true;
    }
  }
  return false;
}

void CalendarActivity::goToMonth(int year, int month) {
  viewYear = year;
  viewMonth = month;
  const int total = daysInMonth(viewYear, viewMonth);
  if (viewYear == todayYear && viewMonth == todayMonth) {
    selectedDay = todayDay;
  } else {
    selectedDay = std::min(selectedDay, total + 1);
  }
  loadNotes();
  ensureHolidaysForYear(viewYear);
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
  state = CalendarState::Grid;
  pendingUpdate = false;
  fetchTaskHandle = nullptr;
  loadNotes();
  ensureHolidaysForYear(viewYear);
  requestUpdate();
}

void CalendarActivity::onExit() {
  Activity::onExit();
  cancelHolidaysFetch();
}

void CalendarActivity::loop() {
  if (pendingUpdate) {
    pendingUpdate = false;
    fetchTaskHandle = nullptr;
    int year = fetchingYear;
    holidaysRefreshing = false;
    if (backgroundFetchSuccess) {
      Storage.remove(holidaysCachePath(year).c_str());
      Storage.rename(holidaysTmpPath(year).c_str(), holidaysCachePath(year).c_str());
    }
    // startHolidaysFetch() doesn't clear `holidays` up front (unlike the
    // heavier apps this pattern is modeled on) since a single year's payload
    // is only ~2KB and never competes with mbedTLS's TLS buffers, but the
    // reload must still happen unconditionally so a failed refresh restores
    // whatever was last cached instead of leaving stale in-memory data.
    loadHolidaysFromSd(year);
    if (!holidaysLoaded && holidaysError.empty()) {
      holidaysError = tr(STR_CALENDAR_HOLIDAYS_NO_DATA);
    } else if (holidaysLoaded && !backgroundFetchSuccess) {
      holidaysRefreshFailed = true;
    }
    requestUpdate();
  }

  using Button = MappedInputManager::Button;

  if (state == CalendarState::HolidayList) {
    const int totalRows = 1 + static_cast<int>(holidays.size());  // row 0 = synthetic "Refresh"
    if (mappedInput.wasReleased(Button::Back)) {
      state = CalendarState::Grid;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Up)) {
      holidayListSelectedRow = (holidayListSelectedRow - 1 + totalRows) % totalRows;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      holidayListSelectedRow = (holidayListSelectedRow + 1) % totalRows;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      if (holidayListSelectedRow == 0) {
        startHolidaysFetch(viewYear, /*promptWifi=*/true);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(Button::Back)) {
    finish();
    return;
  }

  const int total = daysInMonth(viewYear, viewMonth);
  const int totalWithHolidaysRow = total + 1;  // last position = "View Holidays"

  if (mappedInput.wasReleased(Button::Up)) {
    selectedDay = selectedDay > 1 ? selectedDay - 1 : totalWithHolidaysRow;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Down)) {
    selectedDay = selectedDay < totalWithHolidaysRow ? selectedDay + 1 : 1;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Left)) {
    int y = viewYear;
    int m = viewMonth - 1;
    if (m < 1) {
      m = 12;
      y--;
    }
    goToMonth(y, m);
  } else if (mappedInput.wasReleased(Button::Right)) {
    int y = viewYear;
    int m = viewMonth + 1;
    if (m > 12) {
      m = 1;
      y++;
    }
    goToMonth(y, m);
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    if (selectedDay == totalWithHolidaysRow) {
      holidayListSelectedRow = 0;
      state = CalendarState::HolidayList;
      requestUpdate();
    } else {
      openNoteEditor();
    }
  }
}

void CalendarActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == CalendarState::HolidayList) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                  tr(STR_CALENDAR_HOLIDAYS_TITLE));

    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (holidaysRefreshing) {
      const int textY = listTop + (listBottom - listTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_CALENDAR_HOLIDAYS_LOADING));
    } else {
      const int totalRows = 1 + static_cast<int>(holidays.size());
      // Sorted view over `holidays` by (month, day) without mutating storage order.
      std::vector<int> order(holidays.size());
      for (size_t i = 0; i < order.size(); i++) order[i] = static_cast<int>(i);
      std::sort(order.begin(), order.end(), [this](int a, int b) {
        const auto& ha = holidays[a];
        const auto& hb = holidays[b];
        return ha.month != hb.month ? ha.month < hb.month : ha.day < hb.day;
      });

      GUI.drawList(
          renderer, Rect{0, listTop, pageWidth, listBottom - listTop}, totalRows, holidayListSelectedRow,
          [this, &order](int i) -> std::string {
            if (i == 0) return tr(STR_CALENDAR_REFRESH);
            return holidays[order[i - 1]].name;
          },
          [this, &order](int i) -> std::string {
            if (i == 0) return "";
            const auto& h = holidays[order[i - 1]];
            char buf[24];
            snprintf(buf, sizeof(buf), "%d %s", h.day, I18N.get(kMonthKeys[h.month - 1]));
            return buf;
          },
          nullptr, nullptr, false);

      if (holidaysRefreshFailed) {
        GUI.drawPopup(renderer, tr(STR_CALENDAR_HOLIDAYS_REFRESH_FAILED), false);
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

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
    const bool dayIsHoliday = isHoliday(viewMonth, day, nullptr);

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

    // Two fixed, non-overlapping marker positions so a day that is both a
    // holiday and has a personal note shows both: public holiday at the top,
    // personal note at the bottom (matching where the info bar shows each).
    if (dayIsHoliday) {
      renderer.fillRect(cx + cellW / 2 - 2, cy + 3, 4, 4, !isSelected);
    }
    if (hasNote) {
      const int dotY = cy + cellH - 7;
      renderer.fillRect(cx + cellW / 2 - 2, dotY, 4, 4, !isSelected);
    }
  }

  const int noteY = gridBottom + 6;
  if (dateUnconfirmed) {
    renderer.drawText(SMALL_FONT_ID, GRID_MARGIN_X, noteY, "Clock not synced - connect WiFi to fix today's date",
                      true);
  } else if (selectedDay == total + 1) {
    // This bar only ever renders while it's the current selection (it's the
    // info-bar content for that state), so the fill+inverted-text pairing
    // used for a "selected" row elsewhere in this screen is unconditional here.
    renderer.fillRect(GRID_MARGIN_X, noteY - 4, gridW, NOTE_AREA_H - 6, true);
    renderer.drawText(SMALL_FONT_ID, GRID_MARGIN_X + 6, noteY, tr(STR_CALENDAR_VIEW_HOLIDAYS), false);
  } else {
    std::string holidayName;
    const bool selectedIsHoliday = isHoliday(viewMonth, selectedDay, &holidayName);
    const std::string note = getNote(selectedDay);

    std::string preview;
    if (selectedIsHoliday && !note.empty()) {
      preview = holidayName + " - " + note;
    } else if (selectedIsHoliday) {
      preview = holidayName;
    } else {
      preview = note;
    }

    if (!preview.empty()) {
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
