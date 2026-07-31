#include "FormulaOneActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"

namespace {
constexpr const char* API_URLS[3] = {
    "https://api.jolpi.ca/ergast/f1/current/driverstandings/",
    "https://api.jolpi.ca/ergast/f1/current/constructorstandings/",
    "https://api.jolpi.ca/ergast/f1/current/last/results/",
};
constexpr const char* CACHE_PATHS[3] = {
    "/apps/f1/drivers.json",
    "/apps/f1/constructors.json",
    "/apps/f1/results.json",
};
constexpr const char* TMP_PATHS[3] = {
    "/apps/f1/drivers.tmp.json",
    "/apps/f1/constructors.tmp.json",
    "/apps/f1/results.tmp.json",
};

void f1FetchTaskFunc(void* param) {
  auto* activity = static_cast<FormulaOneActivity*>(param);
  activity->runBackgroundFetch();
  vTaskDelete(nullptr);
}
}  // namespace

const char* FormulaOneActivity::apiUrl(int tab) { return API_URLS[tab]; }
const char* FormulaOneActivity::cachePath(int tab) { return CACHE_PATHS[tab]; }
const char* FormulaOneActivity::tmpPath(int tab) { return TMP_PATHS[tab]; }

// Debugging aid for machines where USB serial isn't available: mirrors the
// LOG_ERR/LOG_INF breadcrumbs into a small SD file, viewable from the WiFi
// Files page without needing a USB connection at all.
void FormulaOneActivity::appendDebugLog(const std::string& line) {
  String existing = Storage.readFile("/apps/f1/debug.log");
  String entry = existing + String(millis()) + "ms: " + String(line.c_str()) + "\n";
  constexpr size_t MAX_LOG_CHARS = 4000;
  if (entry.length() > MAX_LOG_CHARS) {
    entry = entry.substring(entry.length() - MAX_LOG_CHARS);
  }
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/f1");
  Storage.writeFile("/apps/f1/debug.log", entry);
}

void FormulaOneActivity::runBackgroundFetch() {
  char buf[160];
  snprintf(buf, sizeof(buf), "Fetching tab %d: %s (free heap: %u)", fetchingTab, apiUrl(fetchingTab),
           (unsigned)ESP.getFreeHeap());
  LOG_INF("F1", "%s", buf);
  appendDebugLog(buf);

  // Stream straight to SD instead of buffering the response in a std::string:
  // holding a growing ~10KB string alive at the same time as an active TLS
  // session is what was aborting the device (std::terminate from a failed
  // allocation under -fno-exceptions). downloadToFile keeps the peak memory
  // footprint to just the TLS session + a small fixed read buffer.
  bool success = false;
  int retries = 3;
  while (retries > 0) {
    std::string errDetail;
    const auto result = HttpDownloader::downloadToFile(apiUrl(fetchingTab), tmpPath(fetchingTab), nullptr, nullptr,
                                                        "", "", nullptr, nullptr, &errDetail);
    if (result == HttpDownloader::OK) {
      success = true;
      break;
    }
    retries--;
    snprintf(buf, sizeof(buf), "download failed for tab %d (%s), %d retries left (free heap: %u)", fetchingTab,
             errDetail.c_str(), retries, (unsigned)ESP.getFreeHeap());
    LOG_ERR("F1", "%s", buf);
    appendDebugLog(buf);
    if (retries > 0) delay(1000);
  }
  snprintf(buf, sizeof(buf), "Download tab %d %s (free heap: %u)", fetchingTab, success ? "OK" : "FAILED",
           (unsigned)ESP.getFreeHeap());
  LOG_INF("F1", "%s", buf);
  appendDebugLog(buf);

  backgroundFetchSuccess = success;
  pendingUpdate = true;
}

void FormulaOneActivity::cancelFetchTask() {
  if (fetchTaskHandle != nullptr) {
    TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
    fetchTaskHandle = nullptr;
    vTaskDelete(tempHandle);
  }
  pendingUpdate = false;
}

void FormulaOneActivity::startFetch(int tab) {
  fetchingTab = tab;
  cancelFetchTask();
  // downloadToFile() does not create parent directories itself.
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/f1");
  ensureWifiConnected(
      [this]() {
        backgroundFetchSuccess = false;
        xTaskCreate(f1FetchTaskFunc, "f1_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
      },
      [this]() {
        char buf[96];
        snprintf(buf, sizeof(buf), "WiFi connect cancelled/failed for tab %d", fetchingTab);
        LOG_ERR("F1", "%s", buf);
        appendDebugLog(buf);
        if (!loaded[fetchingTab]) {
          errorMessage[fetchingTab] = tr(STR_F1_WIFI_REQUIRED);
        }
        requestUpdate();
      });
  requestUpdate();
}

bool FormulaOneActivity::loadCacheFromSd(int tab) {
  String input = Storage.readFile(cachePath(tab));
  if (input.length() == 0) return false;
  parseAndStore(tab, std::string(input.c_str()));
  return loaded[tab];
}

void FormulaOneActivity::parseAndStore(int tab, const std::string& json) {
  char heapBuf[96];
  snprintf(heapBuf, sizeof(heapBuf), "Free heap before parse (tab %d, %u bytes JSON): %u", tab,
           (unsigned)json.size(), (unsigned)ESP.getFreeHeap());
  LOG_INF("F1", "%s", heapBuf);
  appendDebugLog(heapBuf);

  // Only keep the fields we actually render — cuts ArduinoJson's parsed-tree
  // memory well below what the full response (URLs, dates of birth, lap
  // times, etc.) would need, which matters a lot on this hardware's RAM.
  JsonDocument filter;
  if (tab == static_cast<int>(F1Tab::Drivers)) {
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["position"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["points"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["givenName"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["familyName"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Constructors"][0]["name"] = true;
  } else if (tab == static_cast<int>(F1Tab::Constructors)) {
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["position"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["points"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["wins"] = true;
    filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["Constructor"]["name"] = true;
  } else {
    filter["MRData"]["RaceTable"]["Races"][0]["raceName"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["date"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["position"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["points"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Driver"]["familyName"] = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"] = true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (err) {
    char buf[160];
    snprintf(buf, sizeof(buf), "JSON parse failed for tab %d (%u bytes): %s", tab, (unsigned)json.size(), err.c_str());
    LOG_ERR("F1", "%s", buf);
    appendDebugLog(buf);
    errorMessage[tab] = tr(STR_F1_NO_DATA);
    return;
  }

  std::vector<F1Row> newRows;

  if (tab == static_cast<int>(F1Tab::Drivers)) {
    JsonArray standings = doc["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"];
    newRows.reserve(standings.size());
    for (JsonObject item : standings) {
      std::string pos = item["position"] | "";
      std::string given = item["Driver"]["givenName"] | "";
      std::string family = item["Driver"]["familyName"] | "";
      std::string team = item["Constructors"][0]["name"] | "";
      std::string points = item["points"] | "0";
      newRows.push_back(F1Row{pos + ". " + given + " " + family, team, points});
    }
  } else if (tab == static_cast<int>(F1Tab::Constructors)) {
    JsonArray standings = doc["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"];
    newRows.reserve(standings.size());
    for (JsonObject item : standings) {
      std::string pos = item["position"] | "";
      std::string name = item["Constructor"]["name"] | "";
      std::string wins = item["wins"] | "0";
      std::string points = item["points"] | "0";
      newRows.push_back(F1Row{pos + ". " + name, wins + " wins", points});
    }
  } else {
    JsonObject race = doc["MRData"]["RaceTable"]["Races"][0];
    raceName = race["raceName"] | "";
    raceDate = race["date"] | "";
    JsonArray results = race["Results"];
    newRows.reserve(results.size());
    for (JsonObject item : results) {
      std::string pos = item["position"] | "";
      std::string family = item["Driver"]["familyName"] | "";
      std::string team = item["Constructor"]["name"] | "";
      std::string points = item["points"] | "0";
      newRows.push_back(F1Row{pos + ". " + family, team, points});
    }
  }

  if (newRows.empty()) {
    char buf[96];
    snprintf(buf, sizeof(buf), "Parsed JSON for tab %d but found 0 rows (unexpected shape?)", tab);
    LOG_ERR("F1", "%s", buf);
    appendDebugLog(buf);
    errorMessage[tab] = tr(STR_F1_NO_DATA);
    return;
  }

  rows[tab] = std::move(newRows);
  loaded[tab] = true;
  errorMessage[tab].clear();

  snprintf(heapBuf, sizeof(heapBuf), "Free heap after parse (tab %d, %u rows): %u", tab,
           (unsigned)rows[tab].size(), (unsigned)ESP.getFreeHeap());
  LOG_INF("F1", "%s", heapBuf);
  appendDebugLog(heapBuf);
}

void FormulaOneActivity::onEnter() {
  Activity::onEnter();
  pendingUpdate = false;
  fetchTaskHandle = nullptr;

  for (int tab = 0; tab < 3; tab++) {
    if (!loaded[tab]) {
      loadCacheFromSd(tab);
    }
  }

  if (!loaded[static_cast<int>(currentTab)]) {
    startFetch(static_cast<int>(currentTab));
  }

  requestUpdate();
}

void FormulaOneActivity::onExit() {
  Activity::onExit();
  cancelFetchTask();
}

void FormulaOneActivity::loop() {
  if (pendingUpdate) {
    pendingUpdate = false;
    fetchTaskHandle = nullptr;
    int tab = fetchingTab;
    if (backgroundFetchSuccess) {
      // Only replace the existing cache once the fresh download is fully on
      // disk, so a failed refresh never destroys a previously-working cache.
      Storage.remove(cachePath(tab));
      Storage.rename(tmpPath(tab), cachePath(tab));
      loadCacheFromSd(tab);
      if (!loaded[tab] && errorMessage[tab].empty()) {
        errorMessage[tab] = tr(STR_F1_NO_DATA);
      }
    } else if (!loaded[tab]) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Fetch failed for tab %d after retries", tab);
      LOG_ERR("F1", "%s", buf);
      appendDebugLog(buf);
      errorMessage[tab] = tr(STR_F1_NO_DATA);
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    currentTab = static_cast<F1Tab>(ButtonNavigator::previousIndex(static_cast<int>(currentTab), 3));
    int tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    currentTab = static_cast<F1Tab>(ButtonNavigator::nextIndex(static_cast<int>(currentTab), 3));
    int tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    int tab = static_cast<int>(currentTab);
    if (!rows[tab].empty()) {
      selectedRow[tab] = static_cast<int>((selectedRow[tab] - 1 + rows[tab].size()) % rows[tab].size());
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    int tab = static_cast<int>(currentTab);
    if (!rows[tab].empty()) {
      selectedRow[tab] = static_cast<int>((selectedRow[tab] + 1) % rows[tab].size());
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startFetch(static_cast<int>(currentTab));
  }
}

void FormulaOneActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_F1_TITLE));

  const int tabBarY = metrics.topPadding + metrics.headerHeight;
  std::vector<TabInfo> tabs = {
      {tr(STR_F1_TAB_DRIVERS), currentTab == F1Tab::Drivers},
      {tr(STR_F1_TAB_CONSTRUCTORS), currentTab == F1Tab::Constructors},
      {tr(STR_F1_TAB_RESULTS), currentTab == F1Tab::Results},
  };
  GUI.drawTabBar(renderer, Rect{0, tabBarY, pageWidth, metrics.tabBarHeight}, tabs, true);

  int contentTop = tabBarY + metrics.tabBarHeight + metrics.verticalSpacing;

  if (currentTab == F1Tab::Results && !raceName.empty()) {
    const int subHeaderHeight = 30;
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, subHeaderHeight}, raceName.c_str(), raceDate.c_str());
    contentTop += subHeaderHeight + metrics.verticalSpacing;
  }

  const int tab = static_cast<int>(currentTab);
  const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (!loaded[tab]) {
    int textY = contentTop + (listBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    const char* msg = !errorMessage[tab].empty() ? errorMessage[tab].c_str() : tr(STR_F1_LOADING);
    renderer.drawCenteredText(UI_12_FONT_ID, textY, msg);
  } else {
    const auto& tabRows = rows[tab];
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, listBottom - contentTop}, static_cast<int>(tabRows.size()),
        selectedRow[tab], [&tabRows](int i) { return tabRows[i].title; },
        [&tabRows](int i) { return tabRows[i].subtitle; }, nullptr,
        [&tabRows](int i) { return tabRows[i].value; }, true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_F1_REFRESH), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
