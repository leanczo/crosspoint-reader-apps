#include "FootballActivity.h"

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
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/DownloadWatchdog.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"

namespace {

// Short per-attempt timeout for these small JSON list fetches (paired with the
// existing 3x retry loop in runBackgroundFetch()), so a stalled request is
// noticed well within cancelFetchTask()'s cooperative-cancel wait window
// instead of forcing a vTaskDelete() on a task that may hold the storage mutex.
constexpr uint32_t kListFetchTimeoutMs = 8000;

struct AvailableLeague {
  const char* slug;
  const char* name;
};

// Curated so users pick from a list instead of typing a raw ESPN slug (this
// device has no physical keyboard). Slugs verified against ESPN's public,
// keyless soccer scoreboard/standings endpoints.
constexpr AvailableLeague AVAILABLE_LEAGUES[] = {
    {"arg.1", "Liga Profesional Argentina"},
    {"conmebol.libertadores", "Copa Libertadores"},
    {"conmebol.sudamericana", "Copa Sudamericana"},
    {"eng.1", "Premier League"},
    {"esp.1", "La Liga"},
    {"ita.1", "Serie A"},
    {"ger.1", "Bundesliga"},
    {"fra.1", "Ligue 1"},
    {"uefa.champions", "UEFA Champions League"},
    {"uefa.europa", "UEFA Europa League"},
    {"bra.1", "Brasileir\xC3\xA3o"},
    {"mex.1", "Liga MX"},
    {"usa.1", "MLS"},
};
constexpr int NUM_AVAILABLE_LEAGUES = sizeof(AVAILABLE_LEAGUES) / sizeof(AVAILABLE_LEAGUES[0]);

std::string sanitizeSlug(const std::string& slug) {
  std::string s = slug;
  for (char& c : s) {
    if (c == '.') c = '_';
  }
  return s;
}

struct LocalEventTime {
  int month, day, hour, minute;
  bool valid;
};

// ESPN dates are ISO-8601 UTC, e.g. "2026-08-01T18:30Z". Shifts to the same
// UTC offset the Clock app uses (SETTINGS.clockUtcOffsetQ, in quarter-hours —
// configured once in Settings, shared here instead of a football-specific
// setting) and rolls the calendar fields by hand. Deliberately NOT using
// mktime/timegm for this: CalendarActivity avoids them for the same reason
// (they'd pull in libc's unconfigured timezone state on this target).
LocalEventTime toLocalEventTime(const std::string& iso) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0;
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) {
    return LocalEventTime{0, 0, 0, 0, false};
  }

  const int offsetMinutes = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  int totalMinutes = h * 60 + mi + offsetMinutes;

  auto daysInMonth = [](int year, int month) {
    static const int base[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return base[month - 1];
  };

  while (totalMinutes < 0) {
    totalMinutes += 24 * 60;
    d--;
    if (d < 1) {
      mo--;
      if (mo < 1) {
        mo = 12;
        y--;
      }
      d = daysInMonth(y, mo);
    }
  }
  while (totalMinutes >= 24 * 60) {
    totalMinutes -= 24 * 60;
    d++;
    if (d > daysInMonth(y, mo)) {
      d = 1;
      mo++;
      if (mo > 12) {
        mo = 1;
        y++;
      }
    }
  }

  return LocalEventTime{mo, d, totalMinutes / 60, totalMinutes % 60, true};
}

std::string formatEventTimeOnly(const std::string& iso) {
  const LocalEventTime t = toLocalEventTime(iso);
  if (!t.valid) return "";
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
  return buf;
}

std::string formatEventDateOnly(const std::string& iso) {
  const LocalEventTime t = toLocalEventTime(iso);
  if (!t.valid) return "";
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d/%02d", t.month, t.day);
  return buf;
}

// ESPN's status.type.description field comes straight from the API and is
// always in English regardless of the device's language setting. Translate
// the fixed set of values it's actually observed to send (verified against
// the live soccer scoreboard endpoint); anything unrecognized falls back to
// ESPN's raw text instead of showing nothing.
std::string translateMatchStatus(const std::string& desc) {
  if (desc == "Scheduled") return tr(STR_FOOTBALL_STATUS_SCHEDULED);
  if (desc == "Full Time") return tr(STR_FOOTBALL_STATUS_FULL_TIME);
  if (desc == "Halftime") return tr(STR_FOOTBALL_STATUS_HALFTIME);
  if (desc == "In Progress") return tr(STR_FOOTBALL_STATUS_IN_PROGRESS);
  if (desc == "Postponed") return tr(STR_FOOTBALL_STATUS_POSTPONED);
  if (desc == "Canceled" || desc == "Cancelled") return tr(STR_FOOTBALL_STATUS_CANCELED);
  return desc;
}

void footballFetchTaskFunc(void* param) {
  auto* activity = static_cast<FootballActivity*>(param);
  activity->runBackgroundFetch();
  vTaskDelete(nullptr);
}

}  // namespace

std::string FootballActivity::cachePath(int tab) const {
  const std::string slug = sanitizeSlug(subscriptions[activeLeagueIndex].slug);
  return "/apps/football/" + slug + (tab == static_cast<int>(FootballTab::Standings) ? "_standings.json" : "_results.json");
}

std::string FootballActivity::tmpPath(int tab) const {
  const std::string slug = sanitizeSlug(subscriptions[activeLeagueIndex].slug);
  return "/apps/football/" + slug +
         (tab == static_cast<int>(FootballTab::Standings) ? "_standings.tmp.json" : "_results.tmp.json");
}

std::string FootballActivity::apiUrl(int tab) const {
  const std::string& slug = subscriptions[activeLeagueIndex].slug;
  if (tab == static_cast<int>(FootballTab::Standings)) {
    return "https://site.api.espn.com/apis/v2/sports/soccer/" + slug + "/standings";
  }
  // Scoreboard defaults to "today" only, which is often empty/scheduled-only;
  // requesting a trailing window reliably surfaces recently completed matches.
  // Keep the window short: a 10-day range measured ~327KB of raw JSON in the
  // field, which is a lot of WiFi time/battery to throw away after filtering
  // it down to a handful of rows.
  const time_t now = time(nullptr);
  struct tm endTm, startTm;
  const time_t startTime = now - 2 * 86400;
  gmtime_r(&now, &endTm);
  gmtime_r(&startTime, &startTm);
  char endBuf[9], startBuf[9];
  snprintf(endBuf, sizeof(endBuf), "%04d%02d%02d", endTm.tm_year + 1900, endTm.tm_mon + 1, endTm.tm_mday);
  snprintf(startBuf, sizeof(startBuf), "%04d%02d%02d", startTm.tm_year + 1900, startTm.tm_mon + 1, startTm.tm_mday);
  return "https://site.api.espn.com/apis/site/v2/sports/soccer/" + slug + "/scoreboard?dates=" + startBuf + "-" + endBuf;
}

// Debugging aid for machines where USB serial isn't available: mirrors the
// LOG_ERR/LOG_INF breadcrumbs into a small SD file, viewable from the WiFi
// Files page without needing a USB connection at all.
void FootballActivity::appendDebugLog(const std::string& line) {
  String existing = Storage.readFile("/apps/football/debug.log");
  String entry = existing + String(millis()) + "ms: " + String(line.c_str()) + "\n";
  constexpr size_t MAX_LOG_CHARS = 4000;
  if (entry.length() > MAX_LOG_CHARS) {
    entry = entry.substring(entry.length() - MAX_LOG_CHARS);
  }
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/football");
  Storage.writeFile("/apps/football/debug.log", entry);
}

void FootballActivity::loadSubscriptions() {
  subscriptions.clear();
  String content = Storage.readFile("/apps/football/leagues.txt");
  std::string data(content.c_str());

  size_t pos = 0;
  while (pos < data.length()) {
    size_t nl = data.find('\n', pos);
    std::string line = (nl == std::string::npos) ? data.substr(pos) : data.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t sep = line.find('|');
    if (!line.empty() && sep != std::string::npos) {
      subscriptions.push_back(FootballLeague{line.substr(0, sep), line.substr(sep + 1)});
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }

  if (subscriptions.empty()) {
    subscriptions.push_back(FootballLeague{"arg.1", "Liga Profesional Argentina"});
    saveSubscriptions();
  }
}

void FootballActivity::saveSubscriptions() {
  String content;
  for (const auto& league : subscriptions) {
    content += String(league.slug.c_str()) + "|" + String(league.displayName.c_str()) + "\n";
  }
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/football");
  Storage.writeFile("/apps/football/leagues.txt", content);
}

void FootballActivity::rebuildAddLeagueList() {
  addLeagueAvailableIndices.clear();
  for (int i = 0; i < NUM_AVAILABLE_LEAGUES; i++) {
    bool alreadySubscribed = false;
    for (const auto& league : subscriptions) {
      if (league.slug == AVAILABLE_LEAGUES[i].slug) {
        alreadySubscribed = true;
        break;
      }
    }
    if (!alreadySubscribed) {
      addLeagueAvailableIndices.push_back(i);
    }
  }
  if (selectedAddIndex >= static_cast<int>(addLeagueAvailableIndices.size())) {
    selectedAddIndex = 0;
  }
}

void FootballActivity::runBackgroundFetch() {
  // Guards against a genuinely stuck HTTP call (observed as the whole device
  // becoming unresponsive on refresh): forcibly disconnects WiFi after 60s,
  // which unblocks any in-flight esp_http_client call so this task can still
  // report failure instead of hanging forever. Same protection RSS's fetch has.
  DownloadWatchdog::start(60000);

  // The Results tab's URL embeds a trailing-window date range computed from
  // time(nullptr); without a synced clock that's ~seconds-since-boot, which
  // built a bogus "dates=19691222-19700101" query in the wild — ESPN accepts
  // that range fine and just returns 0 events, which then looked like "no
  // data" rather than a clock problem. Standings doesn't need dates, so only
  // bail out for Results when the sync genuinely fails.
  const bool timeSynced = WifiConnectHelper::waitForTimeSync();
  if (fetchingTab == static_cast<int>(FootballTab::Results) && !timeSynced) {
    LOG_ERR("FOOTBALL", "Clock sync failed; skipping Results fetch (would use a bogus date range)");
    appendDebugLog("Clock sync failed; skipping Results fetch (would use a bogus date range)");
    DownloadWatchdog::stop();
    backgroundFetchSuccess = false;
    pendingUpdate = true;
    return;
  }

  char buf[160];
  snprintf(buf, sizeof(buf), "Fetching tab %d: %s (free heap: %u)", fetchingTab, apiUrl(fetchingTab).c_str(),
           (unsigned)ESP.getFreeHeap());
  LOG_INF("FOOTBALL", "%s", buf);
  appendDebugLog(buf);

  bool success = false;
  int retries = 3;
  while (retries > 0 && !DownloadWatchdog::gotTimeout && !cancelFetch) {
    std::string errDetail;
    const auto result = HttpDownloader::downloadToFile(apiUrl(fetchingTab), tmpPath(fetchingTab), nullptr,
                                                        &cancelFetch, "", "", nullptr, nullptr, &errDetail,
                                                        kListFetchTimeoutMs);
    if (result == HttpDownloader::OK) {
      success = true;
      break;
    }
    retries--;
    snprintf(buf, sizeof(buf), "download failed for tab %d (%s), %d retries left (free heap: %u)", fetchingTab,
             errDetail.c_str(), retries, (unsigned)ESP.getFreeHeap());
    LOG_ERR("FOOTBALL", "%s", buf);
    appendDebugLog(buf);
    if (retries > 0 && !DownloadWatchdog::gotTimeout && !cancelFetch) delay(1000);
  }

  DownloadWatchdog::stop();
  if (DownloadWatchdog::gotTimeout) {
    LOG_ERR("FOOTBALL", "Fetch watchdog timeout for tab %d", fetchingTab);
    appendDebugLog("Fetch watchdog timeout - WiFi force-disconnected");
    success = false;
  }

  snprintf(buf, sizeof(buf), "Download tab %d %s (free heap: %u)", fetchingTab, success ? "OK" : "FAILED",
           (unsigned)ESP.getFreeHeap());
  LOG_INF("FOOTBALL", "%s", buf);
  appendDebugLog(buf);

  // Cancelled (cancelFetchTask() is waiting on fetchTaskHandle below): clear
  // it ourselves from inside the task, right before it ends, instead of
  // reporting a result nobody asked for anymore.
  if (cancelFetch) {
    fetchTaskHandle = nullptr;
    return;
  }

  backgroundFetchSuccess = success;
  pendingUpdate = true;
}

// Cooperative cancel + bounded wait, same pattern RssActivity.cpp already
// uses successfully: ask the task to stop (cancelFetch), give it up to 5s to
// unwind through its own esp_http_client/SD-file cleanup, and only fall back
// to a raw vTaskDelete() (which — see startFetch()'s old comment — skips
// that cleanup and leaks the socket/TLS session) if it's genuinely stuck.
void FootballActivity::cancelFetchTask() {
  if (fetchTaskHandle != nullptr) {
    cancelFetch = true;
    int waitCount = 0;
    // Bound exceeds kListFetchTimeoutMs (8s) so the cooperative cancel is
    // reliably observed inside a blocked esp_http_client_read() before we'd
    // ever consider the vTaskDelete() fallback below.
    while (fetchTaskHandle != nullptr && waitCount < 1000) {
      delay(10);
      waitCount++;
    }
    if (fetchTaskHandle != nullptr) {
      // Still stuck: it's very likely blocked inside esp_http_client/mbedTLS
      // (DNS, TCP connect, or TLS handshake/read), not just slow. Killing a
      // task via vTaskDelete() there skips its C++ destructors and can catch
      // the heap allocator mid-malloc/free, corrupting it -- surfacing later
      // as an unrelated abort() (see e.g. the crash right after this got
      // force-deleted once, on the next activity's unrelated allocation).
      // Force a WiFi disconnect first -- the same safe unblock mechanism
      // DownloadWatchdog uses -- so the blocked call returns an error and the
      // task can exit through its own vTaskDelete(nullptr) in
      // footballFetchTaskFunc() instead of being killed mid-operation.
      LOG_ERR("FOOTBALL", "Task stuck after cancel wait, forcing WiFi disconnect to unblock it");
      appendDebugLog("Task stuck after cancel wait, forcing WiFi disconnect to unblock it");
      WiFi.disconnect(true);
      waitCount = 0;
      while (fetchTaskHandle != nullptr && waitCount < 300) {
        delay(10);
        waitCount++;
      }
    }
    if (fetchTaskHandle != nullptr) {
      LOG_ERR("FOOTBALL", "Task failed to exit gracefully, forcing vTaskDelete!");
      appendDebugLog("Task failed to exit gracefully, forcing vTaskDelete!");
      TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
      fetchTaskHandle = nullptr;
      vTaskDelete(tempHandle);
    }
  }
  pendingUpdate = false;
}

void FootballActivity::startFetch(int tab) {
  // Cancel (and fully wait out) any previous fetch BEFORE switching
  // fetchingTab to the new tab — runBackgroundFetch() reads fetchingTab live
  // on every retry, so flipping it while an old task might still be mid-retry
  // would make that old task fetch/save into the new tab's paths instead.
  cancelFetchTask();
  fetchingTab = tab;
  cancelFetch = false;
  refreshing[tab] = true;
  refreshFailed[tab] = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/football");

  // Free both tabs' vectors before the TLS handshake: mbedTLS needs a
  // contiguous ~32KB (16KB in + 16KB out) buffer on this PSRAM-less chip, and
  // a resident results/standings list is enough to fragment that away,
  // surfacing as ESP_ERR_HTTP_CONNECT / "Read error". The active tab is
  // reloaded from the (untouched-on-failure) SD cache once the fetch
  // completes below; the other, currently off-screen tab just gets marked
  // unloaded so it lazily reloads from its own cache next time it's viewed.
  const int otherTab = 1 - tab;
  if (otherTab == static_cast<int>(FootballTab::Results)) {
    resultsMatches.clear();
    resultsMatches.shrink_to_fit();
  } else {
    standingsGroups.clear();
    standingsGroups.shrink_to_fit();
  }
  loaded[otherTab] = false;
  errorMessage[otherTab].clear();

  if (tab == static_cast<int>(FootballTab::Results)) {
    resultsMatches.clear();
    resultsMatches.shrink_to_fit();
  } else {
    standingsGroups.clear();
    standingsGroups.shrink_to_fit();
  }

  ensureWifiConnected(
      [this]() {
        backgroundFetchSuccess = false;
        xTaskCreate(footballFetchTaskFunc, "football_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
      },
      [this]() {
        char buf[96];
        snprintf(buf, sizeof(buf), "WiFi connect cancelled/failed for tab %d", fetchingTab);
        LOG_ERR("FOOTBALL", "%s", buf);
        appendDebugLog(buf);
        refreshing[fetchingTab] = false;
        // startFetch() already cleared this tab's vector above; restore it
        // from disk since the fetch never actually started.
        loadCacheFromSd(fetchingTab);
        if (!loaded[fetchingTab]) {
          errorMessage[fetchingTab] = tr(STR_FOOTBALL_WIFI_REQUIRED);
        } else {
          refreshFailed[fetchingTab] = true;
        }
        requestUpdate();
      });
  requestUpdate();
}

bool FootballActivity::loadCacheFromSd(int tab) {
  HalFile file;
  if (!Storage.openFileForRead("FOOTBALL", cachePath(tab).c_str(), file)) {
    return false;
  }
  parseAndStore(tab, file);
  return loaded[tab];
}

void FootballActivity::parseAndStore(int tab, HalFile& file) {
  // ESPN's scoreboard response over a multi-day window can run past 300KB.
  // Reading it whole into a String (then again into a std::string, as this
  // used to do) held two full copies in RAM at once and crashed the device
  // OOM. Feeding the open file straight to ArduinoJson lets it scan the raw
  // bytes a chunk at a time; only the small *filtered* result tree is kept.
  struct HalFileJsonReader {
    HalFile& f;
    int read() { return f.read(); }
    size_t readBytes(char* buffer, size_t length) {
      const int n = f.read(buffer, length);
      return n < 0 ? 0 : static_cast<size_t>(n);
    }
  } reader{file};

  char heapBuf[96];
  snprintf(heapBuf, sizeof(heapBuf), "Free heap before parse (tab %d, %u bytes JSON): %u", tab, (unsigned)file.size(),
           (unsigned)ESP.getFreeHeap());
  LOG_INF("FOOTBALL", "%s", heapBuf);
  appendDebugLog(heapBuf);

  JsonDocument filter;
  if (tab == static_cast<int>(FootballTab::Results)) {
    filter["events"][0]["date"] = true;
    filter["events"][0]["status"]["type"]["state"] = true;
    filter["events"][0]["status"]["type"]["description"] = true;
    filter["events"][0]["competitions"][0]["competitors"][0]["homeAway"] = true;
    filter["events"][0]["competitions"][0]["competitors"][0]["score"] = true;
    filter["events"][0]["competitions"][0]["competitors"][0]["team"]["displayName"] = true;
  } else {
    filter["children"][0]["name"] = true;
    filter["children"][0]["standings"]["entries"][0]["team"]["displayName"] = true;
    filter["children"][0]["standings"]["entries"][0]["stats"][0]["name"] = true;
    filter["children"][0]["standings"]["entries"][0]["stats"][0]["displayValue"] = true;
  }

  JsonDocument doc;
  // ESPN's scoreboard/standings responses nest well past ArduinoJson's default
  // limit of 10 (broadcast/venue/link metadata we filter out still counts
  // toward nesting while being scanned) - raised, or every parse fails with
  // "TooDeep" regardless of how small the filtered-down result is.
  DeserializationError err =
      deserializeJson(doc, reader, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(20));
  if (err) {
    char buf[160];
    snprintf(buf, sizeof(buf), "JSON parse failed for tab %d: %s", tab, err.c_str());
    LOG_ERR("FOOTBALL", "%s", buf);
    appendDebugLog(buf);
    errorMessage[tab] = tr(STR_FOOTBALL_NO_DATA);
    return;
  }

  std::vector<FootballMatch> newMatches;
  std::vector<FootballGroup> newGroups;

  if (tab == static_cast<int>(FootballTab::Results)) {
    constexpr size_t MAX_RESULT_ROWS = 20;  // defensive cap regardless of window size
    JsonArray events = doc["events"];
    newMatches.reserve(std::min(events.size(), MAX_RESULT_ROWS));
    for (JsonObject event : events) {
      if (newMatches.size() >= MAX_RESULT_ROWS) break;
      std::string state = event["status"]["type"]["state"] | "";
      std::string desc = event["status"]["type"]["description"] | "";
      std::string dateStr = event["date"] | "";

      std::string home, away, homeScore, awayScore;
      JsonArray competitors = event["competitions"][0]["competitors"];
      for (JsonObject c : competitors) {
        std::string homeAway = c["homeAway"] | "";
        std::string name = c["team"]["displayName"] | "";
        std::string score = c["score"] | "";
        if (homeAway == "home") {
          home = name;
          homeScore = score;
        } else {
          away = name;
          awayScore = score;
        }
      }

      newMatches.push_back(FootballMatch{home, away, homeScore, awayScore, state, desc, dateStr});
    }
    // Most recently played first: events already arrive in chronological
    // order from ESPN, so just reverse rather than re-sorting.
    std::reverse(newMatches.begin(), newMatches.end());
  } else {
    // ESPN doesn't return entries pre-sorted by rank, so sort each group
    // (zone) separately by rank before appending; groups themselves stay in
    // ESPN's own order and are kept as separate FootballGroups so the UI can
    // show one group's table at a time instead of one long merged list.
    struct StandingRow {
      int rank;
      std::string rankDisplay;
      std::string team;
      std::string points;
    };

    JsonArray children = doc["children"];
    newGroups.reserve(children.size());
    for (JsonObject child : children) {
      std::string groupName = child["name"] | "";
      JsonArray entries = child["standings"]["entries"];

      std::vector<StandingRow> groupRows;
      groupRows.reserve(entries.size());
      for (JsonObject entry : entries) {
        std::string team = entry["team"]["displayName"] | "";
        std::string rankDisplay, points;
        for (JsonObject stat : entry["stats"].as<JsonArray>()) {
          std::string statName = stat["name"] | "";
          if (statName == "rank") {
            rankDisplay = stat["displayValue"] | "";
          } else if (statName == "points") {
            points = stat["displayValue"] | "";
          }
        }
        groupRows.push_back(StandingRow{atoi(rankDisplay.c_str()), rankDisplay, team, points});
      }
      std::sort(groupRows.begin(), groupRows.end(),
                [](const StandingRow& a, const StandingRow& b) { return a.rank < b.rank; });

      FootballGroup group;
      group.name = groupName;
      group.rows.reserve(groupRows.size());
      for (const auto& row : groupRows) {
        group.rows.push_back(FootballRow{row.rankDisplay + ". " + row.team, "", row.points + " pts"});
      }
      newGroups.push_back(std::move(group));
    }
  }

  const bool isResults = tab == static_cast<int>(FootballTab::Results);
  size_t totalRows = isResults ? newMatches.size() : 0;
  if (!isResults) {
    for (const auto& group : newGroups) totalRows += group.rows.size();
  }

  if (totalRows == 0) {
    char buf[96];
    snprintf(buf, sizeof(buf), "Parsed JSON for tab %d but found 0 rows (unexpected shape?)", tab);
    LOG_ERR("FOOTBALL", "%s", buf);
    appendDebugLog(buf);
    errorMessage[tab] = tr(STR_FOOTBALL_NO_DATA);
    return;
  }

  if (isResults) {
    resultsMatches = std::move(newMatches);
    selectedResultsRow = 0;
  } else {
    standingsGroups = std::move(newGroups);
    selectedGroupIndex = 0;
    selectedGroupRow = 0;
  }
  loaded[tab] = true;
  errorMessage[tab].clear();

  snprintf(heapBuf, sizeof(heapBuf), "Free heap after parse (tab %d, %u rows): %u", tab, (unsigned)totalRows,
           (unsigned)ESP.getFreeHeap());
  LOG_INF("FOOTBALL", "%s", heapBuf);
  appendDebugLog(heapBuf);
}

void FootballActivity::onEnter() {
  Activity::onEnter();
  pendingUpdate = false;
  fetchTaskHandle = nullptr;
  loadSubscriptions();
  rebuildAddLeagueList();
  requestUpdate();
}

void FootballActivity::onExit() {
  Activity::onExit();
  cancelFetchTask();
}

void FootballActivity::loop() {
  if (pendingUpdate) {
    pendingUpdate = false;
    fetchTaskHandle = nullptr;
    int tab = fetchingTab;
    refreshing[tab] = false;
    if (backgroundFetchSuccess) {
      Storage.remove(cachePath(tab).c_str());
      Storage.rename(tmpPath(tab).c_str(), cachePath(tab).c_str());
    }
    // startFetch() cleared this tab's vector before the fetch started, so the
    // reload must happen unconditionally — on failure this is the only way
    // to get the old (still-good, untouched-on-disk) data back, since a
    // failed fetch never touches cachePath().
    loadCacheFromSd(tab);
    if (!loaded[tab]) {
      if (errorMessage[tab].empty()) errorMessage[tab] = tr(STR_FOOTBALL_NO_DATA);
    } else if (!backgroundFetchSuccess) {
      // Refresh failed but we still have good cached data from before —
      // keep showing it instead of replacing it with an error screen, but
      // flag it so render() can surface a brief "couldn't update" banner
      // instead of failing completely silently.
      refreshFailed[tab] = true;
    }
    requestUpdate();
  }

  if (state == FootballState::LeagueSelection) {
    const int totalItems = static_cast<int>(subscriptions.size()) + 1;  // + "Add League"
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedSubIndex = (selectedSubIndex - 1 + totalItems) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedSubIndex = (selectedSubIndex + 1) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedSubIndex == totalItems - 1) {
        rebuildAddLeagueList();
        selectedAddIndex = 0;
        state = FootballState::AddLeague;
        requestUpdate();
      } else {
        activeLeagueIndex = selectedSubIndex;
        currentTab = FootballTab::Results;
        state = FootballState::LeagueDetail;
        for (int t = 0; t < 2; t++) {
          loaded[t] = false;
          errorMessage[t].clear();
        }
        resultsMatches.clear();
        selectedResultsRow = 0;
        standingsGroups.clear();
        selectedGroupIndex = 0;
        selectedGroupRow = 0;
        if (!loadCacheFromSd(static_cast<int>(currentTab))) {
          startFetch(static_cast<int>(currentTab));
        }
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (selectedSubIndex >= 0 && selectedSubIndex < totalItems - 1) {
        const FootballLeague toDelete = subscriptions[selectedSubIndex];
        auto handler = [this, toDelete](const ActivityResult& res) {
          if (!res.isCancelled) {
            for (size_t i = 0; i < subscriptions.size(); i++) {
              if (subscriptions[i].slug == toDelete.slug) {
                subscriptions.erase(subscriptions.begin() + i);
                break;
              }
            }
            saveSubscriptions();
            selectedSubIndex = 0;
            const std::string slug = sanitizeSlug(toDelete.slug);
            Storage.remove(("/apps/football/" + slug + "_results.json").c_str());
            Storage.remove(("/apps/football/" + slug + "_standings.json").c_str());
          }
          requestUpdate();
        };
        startActivityForResult(
            std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_FOOTBALL_UNSUBSCRIBE),
                                                    toDelete.displayName),
            handler);
      }
    }
  } else if (state == FootballState::AddLeague) {
    const int totalAvailable = static_cast<int>(addLeagueAvailableIndices.size());
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = FootballState::LeagueSelection;
      requestUpdate();
    } else if (totalAvailable > 0 && mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedAddIndex = (selectedAddIndex - 1 + totalAvailable) % totalAvailable;
      requestUpdate();
    } else if (totalAvailable > 0 && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedAddIndex = (selectedAddIndex + 1) % totalAvailable;
      requestUpdate();
    } else if (totalAvailable > 0 && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const AvailableLeague& picked = AVAILABLE_LEAGUES[addLeagueAvailableIndices[selectedAddIndex]];
      subscriptions.push_back(FootballLeague{picked.slug, picked.name});
      saveSubscriptions();
      selectedSubIndex = static_cast<int>(subscriptions.size()) - 1;
      state = FootballState::LeagueSelection;
      requestUpdate();
    }
  } else if (state == FootballState::LeagueDetail) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelFetchTask();
      state = FootballState::LeagueSelection;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      // Within Standings, multiple groups (zones) act as their own tab strip:
      // Left/Right first walks group-by-group, only falling through to the
      // Results<->Standings toggle once you step off either end. This keeps
      // Left/Right as the single "cycle tabs" gesture instead of needing a
      // separate button for groups, which this hardware doesn't have to spare.
      if (currentTab == FootballTab::Standings && standingsGroups.size() > 1 && selectedGroupIndex > 0) {
        selectedGroupIndex--;
        selectedGroupRow = 0;
      } else {
        currentTab = static_cast<FootballTab>(ButtonNavigator::previousIndex(static_cast<int>(currentTab), 2));
        if (currentTab == FootballTab::Standings && standingsGroups.size() > 1) {
          selectedGroupIndex = static_cast<int>(standingsGroups.size()) - 1;
        }
        selectedGroupRow = 0;
        int tab = static_cast<int>(currentTab);
        if (!loaded[tab] && errorMessage[tab].empty()) {
          if (!loadCacheFromSd(tab)) startFetch(tab);
        }
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (currentTab == FootballTab::Standings && standingsGroups.size() > 1 &&
          selectedGroupIndex < static_cast<int>(standingsGroups.size()) - 1) {
        selectedGroupIndex++;
        selectedGroupRow = 0;
      } else {
        currentTab = static_cast<FootballTab>(ButtonNavigator::nextIndex(static_cast<int>(currentTab), 2));
        if (currentTab == FootballTab::Standings) {
          selectedGroupIndex = 0;
          selectedGroupRow = 0;
        }
        int tab = static_cast<int>(currentTab);
        if (!loaded[tab] && errorMessage[tab].empty()) {
          if (!loadCacheFromSd(tab)) startFetch(tab);
        }
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (currentTab == FootballTab::Standings) {
        if (!standingsGroups.empty()) {
          auto& groupRows = standingsGroups[selectedGroupIndex].rows;
          if (!groupRows.empty()) {
            selectedGroupRow = static_cast<int>((selectedGroupRow - 1 + groupRows.size()) % groupRows.size());
            requestUpdate();
          }
        }
      } else if (!resultsMatches.empty()) {
        selectedResultsRow =
            static_cast<int>((selectedResultsRow - 1 + resultsMatches.size()) % resultsMatches.size());
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (currentTab == FootballTab::Standings) {
        if (!standingsGroups.empty()) {
          auto& groupRows = standingsGroups[selectedGroupIndex].rows;
          if (!groupRows.empty()) {
            selectedGroupRow = static_cast<int>((selectedGroupRow + 1) % groupRows.size());
            requestUpdate();
          }
        }
      } else if (!resultsMatches.empty()) {
        selectedResultsRow = static_cast<int>((selectedResultsRow + 1) % resultsMatches.size());
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      startFetch(static_cast<int>(currentTab));
    }
  }
}

// Paginates the same way LyraTheme::drawButtonMenu does (window of pageItems
// rows centered on the selection, clamped to bounds) since GUI.drawList can't
// be reused here — it only draws plain title/subtitle/value text, not a
// custom score box.
void FootballActivity::drawResultsList(int x, int y, int width, int height) {
  const int totalRows = static_cast<int>(resultsMatches.size());
  if (totalRows == 0) return;

  constexpr int kBoxW = 90;
  constexpr int kBoxH = 32;
  constexpr int kGap = 8;          // gap between a team name and the score box
  constexpr int kSidePadding = 12;
  constexpr int kLineGap = 6;      // gap between the name/box line and the status line below it
  constexpr int kDividerPad = 10;  // padding above/below the divider between matches

  // Box text (score/kickoff time) stays at UI_12 BOLD — it's the element that
  // should keep standing out. Team names are a size down at UI_10: at UI_12
  // too many names were getting truncated against the centered box.
  const int boxLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int nameLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int subLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int rowContentH = std::max(boxLineH, kBoxH) + kLineGap + subLineH;
  const int rowStep = rowContentH + kDividerPad * 2 + 1;  // +1 for the divider line itself

  const int pageItems = std::max(1, height / rowStep);
  int pageStart = selectedResultsRow - (pageItems - 1) / 2;
  pageStart = std::max(0, std::min(pageStart, std::max(0, totalRows - pageItems)));

  int rowY = y;
  for (int i = pageStart; i < totalRows && i < pageStart + pageItems; i++) {
    const auto& m = resultsMatches[i];
    const bool selected = (i == selectedResultsRow);
    const bool isLive = (m.state == "in");
    const bool isPre = (m.state == "pre");

    if (selected) {
      // Full dark row instead of just an outline, so the current selection is
      // unambiguous — everything drawn below flips to white on this row.
      renderer.fillRect(x, rowY - kDividerPad / 2, width, rowContentH + kDividerPad, true);
    }

    const int boxX = x + (width - kBoxW) / 2;
    const int boxY = rowY;
    // The "live = filled black box" distinction only matters when scanning
    // the whole list at a glance; on the row you're already focused on, the
    // box always goes white-filled + black border/text instead, so it still
    // reads clearly against the row's black background.
    const bool boxFilledBlack = isLive && !selected;
    if (boxFilledBlack) {
      renderer.fillRoundedRect(boxX, boxY, kBoxW, kBoxH, 4, Color::Black);
    } else {
      if (selected) {
        renderer.fillRoundedRect(boxX, boxY, kBoxW, kBoxH, 4, Color::White);
      }
      renderer.drawRoundedRect(boxX, boxY, kBoxW, kBoxH, 1, 4, true);
    }

    const std::string boxText = isPre ? formatEventTimeOnly(m.dateIso) : (m.homeScore + " - " + m.awayScore);
    const int boxTextW = renderer.getTextWidth(UI_12_FONT_ID, boxText.c_str(), EpdFontFamily::BOLD);
    const int boxTextX = boxX + (kBoxW - boxTextW) / 2;
    const int boxTextY = boxY + (kBoxH - boxLineH) / 2;
    renderer.drawText(UI_12_FONT_ID, boxTextX, boxTextY, boxText.c_str(), !boxFilledBlack, EpdFontFamily::BOLD);

    // Team names are truncated to whatever room is left on their side of the
    // centered box, so a long name can't run into (or under) the score.
    const int leftAvailW = boxX - kGap - (x + kSidePadding);
    const int rightAvailW = (x + width - kSidePadding) - (boxX + kBoxW + kGap);
    const std::string homeTrunc =
        renderer.truncatedText(UI_10_FONT_ID, m.home.c_str(), leftAvailW, EpdFontFamily::REGULAR);
    const std::string awayTrunc =
        renderer.truncatedText(UI_10_FONT_ID, m.away.c_str(), rightAvailW, EpdFontFamily::REGULAR);
    const int homeTruncW = renderer.getTextWidth(UI_10_FONT_ID, homeTrunc.c_str(), EpdFontFamily::REGULAR);
    const int nameTextY = boxY + (kBoxH - nameLineH) / 2;
    renderer.drawText(UI_10_FONT_ID, boxX - kGap - homeTruncW, nameTextY, homeTrunc.c_str(), !selected,
                      EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, boxX + kBoxW + kGap, nameTextY, awayTrunc.c_str(), !selected,
                      EpdFontFamily::REGULAR);

    const std::string subtitle = translateMatchStatus(m.statusDesc) + "  " + formatEventDateOnly(m.dateIso);
    const int subTextW = renderer.getTextWidth(SMALL_FONT_ID, subtitle.c_str());
    const int subTextX = x + (width - subTextW) / 2;
    const int subTextY = boxY + kBoxH + kLineGap;
    renderer.drawText(SMALL_FONT_ID, subTextX, subTextY, subtitle.c_str(), !selected);

    rowY += rowStep;
    const bool hasNextVisibleRow = (i + 1 < totalRows) && (i + 1 < pageStart + pageItems);
    if (hasNextVisibleRow) {
      const int lineY = rowY - kDividerPad - 1;
      renderer.drawLine(x + kSidePadding, lineY, x + width - kSidePadding, lineY, 1, true);
    }
  }
}

void FootballActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == FootballState::LeagueSelection) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FOOTBALL_TITLE));

    const int totalItems = static_cast<int>(subscriptions.size()) + 1;
    GUI.drawButtonMenu(
        renderer,
        Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
             pageHeight - (metrics.headerHeight + metrics.topPadding + metrics.verticalSpacing + metrics.buttonHintsHeight)},
        totalItems, selectedSubIndex,
        [this, totalItems](int index) {
          if (index == totalItems - 1) return std::string(tr(STR_FOOTBALL_ADD_LEAGUE));
          return subscriptions[index].displayName;
        },
        [this, totalItems](int index) { return index == totalItems - 1 ? UIIcon::File : UIIcon::Football; }, 9);

    const char* rightAction = (selectedSubIndex >= 0 && selectedSubIndex < totalItems - 1) ? tr(STR_FOOTBALL_DELETE) : nullptr;
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, rightAction);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FootballState::AddLeague) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FOOTBALL_ADD_LEAGUE));

    const int totalAvailable = static_cast<int>(addLeagueAvailableIndices.size());
    if (totalAvailable == 0) {
      const int textY = metrics.topPadding + metrics.headerHeight + 60;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_FOOTBALL_ALL_ADDED));
    } else {
      GUI.drawButtonMenu(
          renderer,
          Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
               pageHeight - (metrics.headerHeight + metrics.topPadding + metrics.verticalSpacing + metrics.buttonHintsHeight)},
          totalAvailable, selectedAddIndex,
          [this](int index) { return std::string(AVAILABLE_LEAGUES[addLeagueAvailableIndices[index]].name); },
          [](int) { return UIIcon::Football; }, 9);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), totalAvailable > 0 ? tr(STR_SELECT) : nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const std::string title = subscriptions[activeLeagueIndex].displayName;
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

    const int tabBarY = metrics.topPadding + metrics.headerHeight;

    // When Standings has more than one group (zone), splice its group names
    // into the tab strip after "Resultados" instead of a single "Posiciones"
    // tab, so each group reads as its own page (see Left/Right handling in
    // loop()) rather than one long list with a repeated subtitle per group.
    std::vector<std::string> tabLabels;
    tabLabels.emplace_back(tr(STR_FOOTBALL_TAB_RESULTS));
    int selectedFlatIndex = 0;
    if (standingsGroups.size() > 1) {
      for (const auto& group : standingsGroups) tabLabels.push_back(group.name);
      if (currentTab == FootballTab::Standings) selectedFlatIndex = 1 + selectedGroupIndex;
    } else {
      tabLabels.emplace_back(tr(STR_FOOTBALL_TAB_STANDINGS));
      if (currentTab == FootballTab::Standings) selectedFlatIndex = 1;
    }

    // drawTabBar lays tabs out left-to-right with no scrolling, so a
    // many-group competition (e.g. an 8-group Libertadores stage) would spill
    // off the right edge. Cap it to a small window centered on the active tab.
    const int totalTabs = static_cast<int>(tabLabels.size());
    constexpr int kMaxVisibleTabs = 4;
    const int windowSize = std::min(totalTabs, kMaxVisibleTabs);
    int windowStart = selectedFlatIndex - windowSize / 2;
    windowStart = std::max(0, std::min(windowStart, totalTabs - windowSize));

    std::vector<TabInfo> tabs;
    tabs.reserve(windowSize);
    for (int i = windowStart; i < windowStart + windowSize; i++) {
      tabs.push_back(TabInfo{tabLabels[i].c_str(), i == selectedFlatIndex});
    }
    GUI.drawTabBar(renderer, Rect{0, tabBarY, pageWidth, metrics.tabBarHeight}, tabs, true);

    const int contentTop = tabBarY + metrics.tabBarHeight + metrics.verticalSpacing;
    const int tab = static_cast<int>(currentTab);
    const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const Rect listRect{0, contentTop, pageWidth, listBottom - contentTop};

    if (refreshing[tab]) {
      const int textY = contentTop + (listBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_FOOTBALL_REFRESHING));
    } else if (!loaded[tab]) {
      const int textY = contentTop + (listBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      const char* msg = !errorMessage[tab].empty() ? errorMessage[tab].c_str() : tr(STR_FOOTBALL_LOADING);
      renderer.drawCenteredText(UI_12_FONT_ID, textY, msg);
    } else if (currentTab == FootballTab::Standings) {
      if (!standingsGroups.empty()) {
        // No subtitle callback here: the group name that used to repeat on
        // every row now lives in the tab strip, so this list can use the
        // denser no-subtitle row height instead of wasting space on a blank line.
        const auto& tabRows = standingsGroups[selectedGroupIndex].rows;
        GUI.drawList(
            renderer, listRect, static_cast<int>(tabRows.size()), selectedGroupRow,
            [&tabRows](int i) { return tabRows[i].title; }, nullptr, nullptr,
            [&tabRows](int i) { return tabRows[i].value; }, true);
      }
    } else {
      drawResultsList(listRect.x, listRect.y, listRect.width, listRect.height);
    }

    // refreshing[tab] is handled above (it now hides the list entirely
    // instead of overlaying a popup on top of it), so only the
    // refresh-failed banner can still appear here.
    if (refreshFailed[tab]) {
      GUI.drawPopup(renderer, tr(STR_FOOTBALL_REFRESH_FAILED), false);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_FOOTBALL_REFRESH), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, ButtonArrow::Left,
                        ButtonArrow::Right);
  }

  renderer.displayBuffer();
}
