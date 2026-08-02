#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class HNState { CategoryList, Loading, StoryDetail };
enum class HNTab { Top = 0, New, Ask, Show };
constexpr int HN_TAB_COUNT = 4;

struct HNStory {
  std::string id;
  std::string title;
  std::string url;  // empty for self-text posts (e.g. most Ask HN)
  std::string author;
  int points = 0;
  int numComments = 0;
  std::string relativeTime;
};

class HackerNewsActivity final : public Activity {
 private:
  HNState state = HNState::CategoryList;
  HNTab currentTab = HNTab::Top;

  bool loaded[HN_TAB_COUNT] = {false, false, false, false};
  std::string errorMessage[HN_TAB_COUNT];
  std::vector<HNStory> stories[HN_TAB_COUNT];
  int selectedRow[HN_TAB_COUNT] = {0, 0, 0, 0};

  int detailStoryIndex = -1;  // index into stories[currentTab], set when entering StoryDetail
  int detailMenuIndex = 0;    // which action is highlighted in StoryDetail

  volatile bool pendingUpdate = false;
  bool backgroundFetchSuccess = false;
  int fetchingTab = -1;
  void* fetchTaskHandle = nullptr;
  // Cooperative cancellation for cancelFetchTask() — mirrors FootballActivity/
  // FormulaOneActivity. Plain bool, not volatile: HttpDownloader::downloadToFile's
  // cancelFlag parameter is `bool*`, and passing a `volatile bool*` there wouldn't compile.
  bool cancelFetch = false;

  void startFetch(int tab);
  void cancelFetchTask();
  bool loadCacheFromSd(int tab);
  void parseAndStoreList(int tab, HalFile& file);
  std::string cachePath(int tab) const;
  std::string tmpPath(int tab) const;
  std::string apiUrl(int tab) const;
  static void appendDebugLog(const std::string& line);

  const HNStory* selectedStory() const;
  void openComments();
  void openLink();
  void showQrForLink();

 public:
  void runBackgroundFetch();  // called from the FreeRTOS task trampoline

  explicit HackerNewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HackerNews", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
