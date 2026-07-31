#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class F1Tab { Drivers = 0, Constructors = 1, Results = 2 };

struct F1Row {
  std::string title;
  std::string subtitle;
  std::string value;
};

class FormulaOneActivity final : public Activity {
 private:
  F1Tab currentTab = F1Tab::Drivers;
  int selectedRow[3] = {0, 0, 0};  // remembered scroll position per tab
  bool loaded[3] = {false, false, false};
  std::string errorMessage[3];
  std::vector<F1Row> rows[3];
  std::string raceName;  // Results tab sub-header (data from the API, not i18n)
  std::string raceDate;

  volatile bool pendingUpdate = false;
  bool backgroundFetchSuccess = false;
  int fetchingTab = -1;
  void* fetchTaskHandle = nullptr;

  void startFetch(int tab);
  void cancelFetchTask();
  bool loadCacheFromSd(int tab);
  void parseAndStore(int tab, const std::string& json);
  static const char* cachePath(int tab);
  static const char* tmpPath(int tab);
  static const char* apiUrl(int tab);
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
