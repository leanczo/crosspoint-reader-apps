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

  std::vector<FootballRow> resultsRows;
  int selectedResultsRow = 0;

  std::vector<FootballGroup> standingsGroups;
  int selectedGroupIndex = 0;  // which group tab is active within Standings
  int selectedGroupRow = 0;    // selected row within the active group

  volatile bool pendingUpdate = false;
  bool backgroundFetchSuccess = false;
  int fetchingTab = -1;
  void* fetchTaskHandle = nullptr;

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

 public:
  void runBackgroundFetch();  // called from the FreeRTOS task trampoline

  explicit FootballActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Football", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
