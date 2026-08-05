#pragma once

#include <DateMath.h>
#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class OnThisDayState { List, Loading };
enum class OnThisDayCategory { Events = 0, Births = 1, Deaths = 2 };
constexpr int OTD_CATEGORY_COUNT = 3;

struct OnThisDayEntry {
  int year = 0;
  std::string text;
  std::string pageUrl;  // pages[0].content_urls.desktop.page; may be empty
};

class OnThisDayActivity final : public Activity {
 private:
  OnThisDayState state = OnThisDayState::List;
  OnThisDayCategory currentCategory = OnThisDayCategory::Events;
  // Only month/day are ever read; year is a fixed anchor so DateMath's day
  // arithmetic has a valid CivilDate to round-trip through and is never
  // itself displayed or sent to the API (the onthisday feed is year-independent).
  DateMath::CivilDate viewedDate;

  bool loaded[OTD_CATEGORY_COUNT] = {false, false, false};
  bool refreshing[OTD_CATEGORY_COUNT] = {false, false, false};
  bool refreshFailed[OTD_CATEGORY_COUNT] = {false, false, false};
  std::string errorMessage[OTD_CATEGORY_COUNT];
  std::vector<OnThisDayEntry> entries[OTD_CATEGORY_COUNT];
  // 0=Refresh, 1=PrevDay, 2=NextDay, 3+ = entries[cat][selectedRow-3]
  int selectedRow[OTD_CATEGORY_COUNT] = {0, 0, 0};
  int scrollOffset[OTD_CATEGORY_COUNT] = {0, 0, 0};  // first visible entry index

  // Lowercased UI language code, e.g. "es"; may flip to "en" after a 404
  // (see runBackgroundFetch's one-time language-fallback probe).
  std::string activeLangCode;
  bool langFallbackProbed = false;

  volatile bool pendingUpdate = false;
  bool backgroundFetchSuccess = false;
  int fetchingCategory = -1;
  void* fetchTaskHandle = nullptr;
  bool cancelFetch = false;

  void startFetch(int category);
  void cancelFetchTask();
  bool loadCacheFromSd(int category);
  void parseAndStore(int category, HalFile& file);
  std::string cachePath(int category) const;
  std::string tmpPath(int category) const;
  std::string apiUrl(int category) const;
  static void appendDebugLog(const std::string& line);

  void changeDate(int deltaDays);
  void showQrForSelected();
  std::string formattedHeaderDate() const;

 public:
  void runBackgroundFetch();  // called from the FreeRTOS task trampoline

  explicit OnThisDayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OnThisDay", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
