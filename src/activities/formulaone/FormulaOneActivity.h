#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// SessionSchedule, like Results, isn't a real tab in the bar — it's a detail
// view reached only by confirming a Calendar row for a race that hasn't run
// yet (Results is for one that already has).
enum class F1Tab { Drivers = 0, Constructors = 1, Results = 2, Calendar = 3, SessionSchedule = 4 };
constexpr int F1_TAB_COUNT = 5;

struct F1Row {
  std::string title;
  std::string subtitle;
  std::string value;
};

// Practice/qualifying/sprint session times for one race weekend, parallel to
// calendarRounds/calendarDatesIso. Pulled straight out of the Calendar tab's
// own fetch (the schedule endpoint already includes these) — showing them
// needs no extra network request. Sprint fields are empty on a non-sprint
// weekend; FP2/FP3 are empty on a sprint weekend (only FP1 is scheduled).
struct F1RaceWeekend {
  std::string fp1Date, fp1Time;
  std::string fp2Date, fp2Time;
  std::string fp3Date, fp3Time;
  std::string sprintQualDate, sprintQualTime;
  std::string sprintDate, sprintTime;
  std::string qualDate, qualTime;
};

class FormulaOneActivity final : public Activity {
 private:
  F1Tab currentTab = F1Tab::Drivers;
  int selectedRow[F1_TAB_COUNT] = {0, 0, 0, 0, 0};  // remembered scroll position per tab
  bool loaded[F1_TAB_COUNT] = {false, false, false, false, false};
  std::string errorMessage[F1_TAB_COUNT];
  // Shown as an overlay banner while startFetch() has an in-flight request
  // for that tab, independent of `loaded[tab]` — otherwise a manual refresh
  // of already-loaded data gives zero visual feedback while it's in flight.
  bool refreshing[F1_TAB_COUNT] = {false, false, false, false, false};
  // Set when a manual refresh fails while `loaded[tab]` was already true, so
  // the old data stays on screen but the user still sees that it didn't
  // update, instead of the refresh failing in total silence.
  bool refreshFailed[F1_TAB_COUNT] = {false, false, false, false, false};
  std::vector<F1Row> rows[F1_TAB_COUNT];
  std::string raceName;  // Results tab sub-header (data from the API, not i18n)
  std::string raceDate;

  // Results normally shows the latest race ("current/last/results/", the
  // -1 case). Confirming a past race from the Calendar tab instead drills
  // into that specific round, reusing the same Results slot/cache mechanism
  // parameterized by round number.
  int selectedRound = -1;

  // Round number and raw ISO date per row in rows[Calendar], same order/index
  // — round lets Confirm know which round to fetch; date is what decides
  // whether a race is done yet (compared against today, not against any
  // separately-fetched "last results" — Results isn't a directly reachable
  // tab anymore, so nothing would ever populate that automatically).
  std::vector<int> calendarRounds;
  std::vector<std::string> calendarDatesIso;
  std::vector<F1RaceWeekend> calendarWeekends;  // session times, same order/index
  std::string sessionScheduleRaceName;          // subheader while viewing SessionSchedule
  // One-shot: the first time the Calendar loads, jump the cursor to the most
  // recent already-run race so it's immediately selectable without
  // scrolling. Doesn't fight the user's own scrolling on later visits.
  bool calendarCursorPositioned = false;
  void positionCalendarCursorOnLastPastRace();
  // Builds rows[SessionSchedule] from calendarWeekends[calendarIdx] and
  // switches currentTab to it — synchronous, no fetch involved.
  void showSessionSchedule(int calendarIdx);

  volatile bool pendingUpdate = false;
  bool backgroundFetchSuccess = false;
  int fetchingTab = -1;
  void* fetchTaskHandle = nullptr;
  // Cooperative cancellation for cancelFetchTask() — see its definition for
  // why this replaced a bare vTaskDelete() (RssActivity.cpp has the same
  // pattern already working; this mirrors it). Plain bool, not volatile,
  // matching RssActivity.h — HttpDownloader::downloadToFile's cancelFlag
  // parameter is `bool*`, and passing a `volatile bool*` there wouldn't compile.
  bool cancelFetch = false;

  void startFetch(int tab);
  void cancelFetchTask();
  bool loadCacheFromSd(int tab);
  void parseAndStore(int tab, const std::string& json);
  std::string cachePath(int tab) const;
  std::string tmpPath(int tab) const;
  std::string apiUrl(int tab) const;
  static void appendDebugLog(const std::string& line);

 public:
  void runBackgroundFetch();  // called from the FreeRTOS task trampoline

  explicit FormulaOneActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FormulaOne", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
