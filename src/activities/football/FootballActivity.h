#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class FootballState { LeagueSelection, AddLeague, LeagueDetail };
enum class FootballTab { Results = 0, Standings = 1 };

struct FootballLeague {
  std::string slug;         // ESPN league slug, e.g. "arg.1"
  std::string displayName;  // e.g. "Liga Profesional Argentina"
};

struct FootballRow {
  std::string title;
  std::string subtitle;
  std::string value;
};

// Results keeps the raw fields (not a pre-baked title string) so the row
// renderer can lay the score out in its own centered box instead of just
// concatenating "Home 1 - 4 Away" as plain text.
struct FootballMatch {
  std::string home;
  std::string away;
  std::string homeScore;
  std::string awayScore;
  std::string state;       // ESPN status.type.state: "pre" | "in" | "post"
  std::string statusDesc;  // e.g. "Full Time", "Scheduled", "1st Half"
  std::string dateIso;     // raw ESPN date; formatted (and timezone-shifted) at render time
};

// Standings are organized into zones/groups by ESPN (e.g. "Grupo A"/"Grupo B"
// during a Libertadores group stage). Ungrouped leagues still come back as a
// single group, so callers don't need to special-case the count.
struct FootballGroup {
  std::string name;
  std::vector<FootballRow> rows;
};

class FootballActivity final : public Activity {
 private:
  FootballState state = FootballState::LeagueSelection;
  FootballTab currentTab = FootballTab::Results;

  std::vector<FootballLeague> subscriptions;
  std::vector<int> addLeagueAvailableIndices;  // indices into kAvailableLeagues not yet subscribed
  int selectedSubIndex = 0;
  int selectedAddIndex = 0;
  int activeLeagueIndex = -1;  // index into subscriptions, set when entering LeagueDetail

  bool loaded[2] = {false, false};
  std::string errorMessage[2];
  // Shown as an overlay banner while startFetch() has an in-flight request
  // for that tab, independent of `loaded[tab]` — otherwise a manual refresh
  // of already-loaded data gives zero visual feedback while it's in flight.
  bool refreshing[2] = {false, false};
  // Set when a manual refresh fails while `loaded[tab]` was already true, so
  // the old data stays on screen but the user still sees that it didn't
  // update, instead of the refresh failing in total silence.
  bool refreshFailed[2] = {false, false};

  std::vector<FootballMatch> resultsMatches;
  int selectedResultsRow = 0;

  std::vector<FootballGroup> standingsGroups;
  int selectedGroupIndex = 0;  // which group tab is active within Standings
  int selectedGroupRow = 0;    // selected row within the active group

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

  void loadSubscriptions();
  void saveSubscriptions();
  void rebuildAddLeagueList();

  void startFetch(int tab);
  void cancelFetchTask();
  bool loadCacheFromSd(int tab);
  void parseAndStore(int tab, HalFile& file);
  std::string cachePath(int tab) const;
  std::string tmpPath(int tab) const;
  std::string apiUrl(int tab) const;
  static void appendDebugLog(const std::string& line);

  // Hand-rolled row rendering for Results (team names + a centered
  // score/kickoff-time box + a divider between matches) — GUI.drawList only
  // does plain title/subtitle/value text, not a custom-laid-out row.
  void drawResultsList(int x, int y, int width, int height);

 public:
  void runBackgroundFetch();  // called from the FreeRTOS task trampoline

  explicit FootballActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Football", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
