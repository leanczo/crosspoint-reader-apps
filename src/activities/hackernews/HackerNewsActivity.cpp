#include "HackerNewsActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/util/DownloadWatchdog.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"

namespace {

// Comments nest with unlimited depth on HN; both caps below are defensive
// bounds so a mega-popular thread can't blow the ~380KB heap budget. Replies
// past the depth cap, or comments past the count cap, are simply omitted
// (with a note appended for the count cap) rather than crashing.
constexpr int kMaxCommentDepth = 6;
constexpr int kMaxComments = 200;

// Short per-attempt timeout for these small JSON list fetches (paired with the
// existing 3x retry loop in runBackgroundFetch()), so a stalled request is
// noticed well within cancelFetchTask()'s cooperative-cancel wait window
// instead of forcing a vTaskDelete() on a task that may hold the storage mutex.
constexpr uint32_t kListFetchTimeoutMs = 8000;

std::string sanitizeTitleForFilename(const std::string& input) {
  std::string output;
  for (char c : input) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') {
      output += c;
    }
  }
  if (output.length() > 40) output = output.substr(0, 40);
  while (!output.empty() && output.back() == ' ') output.pop_back();
  if (output.empty()) output = "story";
  return output;
}

std::string formatRelativeTime(int64_t createdAtUnix) {
  time_t now = time(nullptr);
  if (now < static_cast<time_t>(createdAtUnix)) return "";
  int64_t diff = static_cast<int64_t>(now) - createdAtUnix;
  char buf[16];
  if (diff < 60) return tr(STR_HN_TIME_NOW);
  if (diff < 3600) {
    snprintf(buf, sizeof(buf), tr(STR_HN_TIME_MINUTES), static_cast<int>(diff / 60));
  } else if (diff < 86400) {
    snprintf(buf, sizeof(buf), tr(STR_HN_TIME_HOURS), static_cast<int>(diff / 3600));
  } else {
    snprintf(buf, sizeof(buf), tr(STR_HN_TIME_DAYS), static_cast<int>(diff / 86400));
  }
  return buf;
}

// Builds a fixed-depth recursive filter so ArduinoJson only keeps the fields
// we need from an arbitrarily deep (but capped) comment tree - Filter can
// prune known field *paths*, so unlimited recursion is approximated by
// repeating the same shape kMaxCommentDepth times; anything deeper is
// simply absent from the parsed document.
void buildCommentFilter(JsonDocument& filter) {
  JsonObject node = filter.to<JsonObject>();
  node["title"] = true;
  node["url"] = true;
  node["text"] = true;
  node["author"] = true;
  for (int depth = 0; depth < kMaxCommentDepth; depth++) {
    JsonObject child = node["children"][0].to<JsonObject>();
    child["author"] = true;
    child["text"] = true;
    node = child;
  }
}

// Recursively writes each comment as a nested <blockquote>, so
// TxtReaderActivity's existing HTML tokenizer (already used for RSS
// articles) renders reply depth as indentation - zero new HTML/markup
// parsing code needed on the read side.
void writeCommentsRecursive(HalFile& out, JsonArray comments, int& count, bool& truncated) {
  for (JsonObject comment : comments) {
    if (truncated) return;
    if (count >= kMaxComments) {
      truncated = true;
      return;
    }
    std::string text = comment["text"] | "";
    if (text.empty()) continue;  // deleted/flagged comments carry no text
    count++;

    std::string author = comment["author"] | "?";
    out.print("<blockquote><b>");
    out.print(author.c_str());
    out.print(":</b> ");
    out.print(text.c_str());
    out.print("\n");

    JsonArray children = comment["children"];
    writeCommentsRecursive(out, children, count, truncated);
    out.print("</blockquote>\n");
  }
}

void hnFetchTaskFunc(void* param) {
  auto* activity = static_cast<HackerNewsActivity*>(param);
  activity->runBackgroundFetch();
  vTaskDelete(nullptr);
}

}  // namespace

std::string HackerNewsActivity::cachePath(int tab) const {
  static const char* names[HN_TAB_COUNT] = {"top", "new", "ask", "show"};
  return std::string("/apps/hackernews/") + names[tab] + ".json";
}

std::string HackerNewsActivity::tmpPath(int tab) const { return cachePath(tab) + ".tmp"; }

std::string HackerNewsActivity::apiUrl(int tab) const {
  switch (static_cast<HNTab>(tab)) {
    case HNTab::Top:
      return "https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=30";
    case HNTab::New:
      return "https://hn.algolia.com/api/v1/search_by_date?tags=story&hitsPerPage=30";
    case HNTab::Ask:
      return "https://hn.algolia.com/api/v1/search_by_date?tags=ask_hn&hitsPerPage=30";
    case HNTab::Show:
      return "https://hn.algolia.com/api/v1/search_by_date?tags=show_hn&hitsPerPage=30";
  }
  return "";
}

void HackerNewsActivity::appendDebugLog(const std::string& line) {
  String existing = Storage.readFile("/apps/hackernews/debug.log");
  String entry = existing + String(millis()) + "ms: " + String(line.c_str()) + "\n";
  constexpr size_t MAX_LOG_CHARS = 4000;
  if (entry.length() > MAX_LOG_CHARS) {
    entry = entry.substring(entry.length() - MAX_LOG_CHARS);
  }
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/hackernews");
  Storage.writeFile("/apps/hackernews/debug.log", entry);
}

// Cooperative cancel + bounded wait, same pattern FootballActivity/FormulaOneActivity
// use: ask the task to stop (cancelFetch), give it up to 10s to unwind through
// its own esp_http_client/SD-file cleanup, and only fall back to a raw
// vTaskDelete() (which skips that cleanup and can leak the socket/TLS session,
// or leave HalStorage's mutex locked if killed mid-write) if it's genuinely stuck.
void HackerNewsActivity::cancelFetchTask() {
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
      LOG_ERR("HN", "Task failed to exit gracefully, forcing vTaskDelete!");
      appendDebugLog("Task failed to exit gracefully, forcing vTaskDelete!");
      TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
      fetchTaskHandle = nullptr;
      vTaskDelete(tempHandle);
    }
  }
  pendingUpdate = false;
}

void HackerNewsActivity::startFetch(int tab) {
  // Cancel (and fully wait out) any previous fetch BEFORE switching
  // fetchingTab to the new tab — runBackgroundFetch() reads fetchingTab live
  // on every retry, so flipping it while an old task might still be mid-retry
  // would make that old task fetch/save into the new tab's paths instead.
  cancelFetchTask();
  fetchingTab = tab;
  cancelFetch = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/hackernews");
  ensureWifiConnected(
      [this]() {
        backgroundFetchSuccess = false;
        xTaskCreate(hnFetchTaskFunc, "hn_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
      },
      [this]() {
        appendDebugLog("WiFi connect cancelled/failed for tab " + std::to_string(fetchingTab));
        if (!loaded[fetchingTab]) {
          errorMessage[fetchingTab] = tr(STR_HN_WIFI_REQUIRED);
        }
        requestUpdate();
      });
  requestUpdate();
}

void HackerNewsActivity::runBackgroundFetch() {
  DownloadWatchdog::start(60000);
  WifiConnectHelper::waitForTimeSync();

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
    appendDebugLog("List download failed for tab " + std::to_string(fetchingTab) + " (" + errDetail + "), " +
                   std::to_string(retries) + " retries left");
    if (retries > 0 && !DownloadWatchdog::gotTimeout && !cancelFetch) delay(1000);
  }

  DownloadWatchdog::stop();
  if (DownloadWatchdog::gotTimeout) success = false;

  // Cancelled (cancelFetchTask() is waiting on fetchTaskHandle below): clear
  // it ourselves from inside the task, right before it ends, instead of
  // reporting a result nobody asked for anymore.
  if (cancelFetch) {
    fetchTaskHandle = nullptr;
    return;
  }

  // Also clear it here on the success/failure path, before this task
  // self-deletes (see hnFetchTaskFunc). Otherwise, if the user switches tabs
  // before loop() gets around to processing pendingUpdate, cancelFetchTask()
  // finds a handle that looks "live" but whose task has already gone away,
  // spin-waits it out, and forces a vTaskDelete() on a stale handle that
  // FreeRTOS may have since recycled for the next tab's fetch task — killing
  // an unrelated in-flight fetch and leaving that tab stuck.
  fetchTaskHandle = nullptr;
  backgroundFetchSuccess = success;
  pendingUpdate = true;
}

bool HackerNewsActivity::loadCacheFromSd(int tab) {
  HalFile file;
  if (!Storage.openFileForRead("HN", cachePath(tab).c_str(), file)) {
    return false;
  }
  parseAndStoreList(tab, file);
  return loaded[tab];
}

void HackerNewsActivity::parseAndStoreList(int tab, HalFile& file) {
  struct HalFileJsonReader {
    HalFile& f;
    int read() { return f.read(); }
    size_t readBytes(char* buffer, size_t length) {
      const int n = f.read(buffer, length);
      return n < 0 ? 0 : static_cast<size_t>(n);
    }
  } reader{file};

  JsonDocument filter;
  filter["hits"][0]["objectID"] = true;
  filter["hits"][0]["title"] = true;
  filter["hits"][0]["url"] = true;
  filter["hits"][0]["author"] = true;
  filter["hits"][0]["points"] = true;
  filter["hits"][0]["num_comments"] = true;
  filter["hits"][0]["created_at_i"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, reader, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(10));
  if (err) {
    appendDebugLog("JSON parse failed for tab " + std::to_string(tab) + ": " + err.c_str());
    errorMessage[tab] = tr(STR_HN_NO_DATA);
    return;
  }

  std::vector<HNStory> newStories;
  JsonArray hits = doc["hits"];
  newStories.reserve(hits.size());
  for (JsonObject hit : hits) {
    HNStory story;
    story.id = hit["objectID"] | "";
    story.title = hit["title"] | "";
    story.url = hit["url"] | "";
    story.author = hit["author"] | "";
    story.points = hit["points"] | 0;
    story.numComments = hit["num_comments"] | 0;
    story.relativeTime = formatRelativeTime(hit["created_at_i"] | 0);
    if (story.id.empty() || story.title.empty()) continue;
    newStories.push_back(std::move(story));
  }

  if (newStories.empty()) {
    errorMessage[tab] = tr(STR_HN_NO_DATA);
    return;
  }

  // Top ("Populares") is the one tab meant to read as a points ranking; the
  // Algolia API only returns it in front-page relevance order. New/Ask/Show
  // are already requested sorted by date (search_by_date), which is the
  // right order for them, so leave those alone.
  if (tab == static_cast<int>(HNTab::Top)) {
    std::stable_sort(newStories.begin(), newStories.end(),
                     [](const HNStory& a, const HNStory& b) { return a.points > b.points; });
  }

  stories[tab] = std::move(newStories);
  selectedRow[tab] = 0;
  loaded[tab] = true;
  errorMessage[tab].clear();
}

const HNStory* HackerNewsActivity::selectedStory() const {
  int tab = static_cast<int>(currentTab);
  if (detailStoryIndex < 0 || detailStoryIndex >= static_cast<int>(stories[tab].size())) return nullptr;
  return &stories[tab][detailStoryIndex];
}

void HackerNewsActivity::openComments() {
  const HNStory* storyPtr = selectedStory();
  if (!storyPtr) return;
  HNStory story = *storyPtr;  // copy: about to do I/O, don't hold a pointer into stories[]

  std::string destPath = "/apps/hackernews/comments/" + story.id + ".html";
  if (!Storage.exists(destPath.c_str())) {
    GUI.drawPopup(renderer, tr(STR_HN_DOWNLOADING));
    if (!WifiConnectHelper::ensureWifiConnected()) {
      GUI.drawPopup(renderer, tr(STR_HN_WIFI_REQUIRED));
      delay(1500);
      requestUpdate();
      return;
    }
    WifiConnectHelper::waitForTimeSync();

    Storage.ensureDirectoryExists("/apps");
    Storage.ensureDirectoryExists("/apps/hackernews");
    Storage.ensureDirectoryExists("/apps/hackernews/comments");
    std::string tmpJsonPath = "/apps/hackernews/comments_tmp.json";
    std::string url = "https://hn.algolia.com/api/v1/items/" + story.id;

    bool downloadOk = false;
    int retries = 3;
    std::string errDetail;
    while (retries > 0) {
      auto result =
          HttpDownloader::downloadToFile(url, tmpJsonPath, nullptr, nullptr, "", "", nullptr, nullptr, &errDetail);
      if (result == HttpDownloader::OK) {
        downloadOk = true;
        break;
      }
      retries--;
      if (retries > 0) delay(1000);
    }

    bool ok = false;
    if (downloadOk) {
      HalFile jsonFile;
      if (Storage.openFileForRead("HN", tmpJsonPath.c_str(), jsonFile)) {
        struct HalFileJsonReader {
          HalFile& f;
          int read() { return f.read(); }
          size_t readBytes(char* buffer, size_t length) {
            const int n = f.read(buffer, length);
            return n < 0 ? 0 : static_cast<size_t>(n);
          }
        } reader{jsonFile};

        JsonDocument filter;
        buildCommentFilter(filter);
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, reader, DeserializationOption::Filter(filter),
                                                   DeserializationOption::NestingLimit(kMaxCommentDepth + 4));
        if (!err) {
          HalFile outFile;
          if (Storage.openFileForWrite("HN", destPath.c_str(), outFile)) {
            outFile.print("<html><body>\n<h1>");
            std::string title = doc["title"] | story.title;
            outFile.print(title.c_str());
            outFile.print("</h1>\n");

            std::string selfText = doc["text"] | "";
            if (!selfText.empty()) {
              outFile.print("<p>");
              outFile.print(selfText.c_str());
              outFile.print("</p>\n");
            }

            JsonArray topLevel = doc["children"];
            if (topLevel.size() == 0) {
              outFile.print("<p>");
              outFile.print(tr(STR_HN_NO_COMMENTS));
              outFile.print("</p>\n");
            } else {
              int count = 0;
              bool truncated = false;
              writeCommentsRecursive(outFile, topLevel, count, truncated);
              if (truncated) {
                outFile.print("<p><i>");
                outFile.print(tr(STR_HN_COMMENTS_TRUNCATED));
                outFile.print("</i></p>\n");
              }
            }
            outFile.print("</body></html>\n");
            ok = true;
          }
        } else {
          appendDebugLog("Comments JSON parse failed for " + story.id + ": " + err.c_str());
        }
      }
    } else {
      appendDebugLog("Comments download failed for " + story.id + " (" + errDetail + ")");
    }

    Storage.remove(tmpJsonPath.c_str());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    if (!ok) {
      GUI.drawPopup(renderer, tr(STR_HN_DOWNLOAD_FAILED));
      delay(2000);
      requestUpdate();
      return;
    }
  }

  activityManager.pushReader(destPath);
}

void HackerNewsActivity::openLink() {
  const HNStory* storyPtr = selectedStory();
  if (!storyPtr || storyPtr->url.empty()) return;
  HNStory story = *storyPtr;  // copy before I/O

  std::string downloadUrl = story.url;
  for (const std::string& prefix : {std::string("https://www.reddit.com/"), std::string("https://reddit.com/")}) {
    if (downloadUrl.rfind(prefix, 0) == 0) {
      downloadUrl = "https://old.reddit.com/" + downloadUrl.substr(prefix.length());
      break;
    }
  }

  GUI.drawPopup(renderer, tr(STR_HN_DOWNLOADING));
  if (!WifiConnectHelper::ensureWifiConnected()) {
    GUI.drawPopup(renderer, tr(STR_HN_WIFI_REQUIRED));
    delay(1500);
    requestUpdate();
    return;
  }

  std::string sanitized = sanitizeTitleForFilename(story.title);
  Storage.ensureDirectoryExists("/websites");
  std::string tempPath = "/websites/temp_download.tmp";

  bool success = false;
  int retries = 3;
  std::string errDetail;
  while (retries > 0) {
    GUI.drawPopup(renderer, tr(STR_HN_DOWNLOADING));
    auto result =
        HttpDownloader::downloadToFile(downloadUrl, tempPath, nullptr, nullptr, "", "", nullptr, nullptr, &errDetail);
    if (result == HttpDownloader::OK) {
      success = true;
      break;
    }
    retries--;
    if (retries > 0) delay(1000);
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  if (!success) {
    appendDebugLog("Open link failed: " + downloadUrl + " (" + errDetail + ")");
    GUI.drawPopup(renderer, tr(STR_HN_DOWNLOAD_FAILED));
    delay(2000);
    requestUpdate();
    return;
  }

  std::string destPath = "/websites/" + sanitized + ".html";
  {
    RenderLock lock;
    if (Storage.exists(destPath.c_str())) Storage.remove(destPath.c_str());
    Storage.rename(tempPath.c_str(), destPath.c_str());
  }
  activityManager.pushReader(destPath);
}

void HackerNewsActivity::showQrForLink() {
  const HNStory* storyPtr = selectedStory();
  if (!storyPtr || storyPtr->url.empty()) return;
  std::string url = storyPtr->url;
  startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, url),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void HackerNewsActivity::onEnter() {
  Activity::onEnter();
  pendingUpdate = false;
  fetchTaskHandle = nullptr;
  int tab = static_cast<int>(currentTab);
  if (!loadCacheFromSd(tab)) {
    startFetch(tab);
  }
  requestUpdate();
}

void HackerNewsActivity::onExit() {
  Activity::onExit();
  cancelFetchTask();
}

void HackerNewsActivity::loop() {
  if (pendingUpdate) {
    pendingUpdate = false;
    fetchTaskHandle = nullptr;
    int tab = fetchingTab;
    if (backgroundFetchSuccess) {
      Storage.remove(cachePath(tab).c_str());
      Storage.rename(tmpPath(tab).c_str(), cachePath(tab).c_str());
      loadCacheFromSd(tab);
      if (!loaded[tab] && errorMessage[tab].empty()) {
        errorMessage[tab] = tr(STR_HN_NO_DATA);
      }
    } else if (!loaded[tab]) {
      errorMessage[tab] = tr(STR_HN_NO_DATA);
    }
    requestUpdate();
  }

  using Button = MappedInputManager::Button;

  if (state == HNState::StoryDetail) {
    if (mappedInput.wasReleased(Button::Back)) {
      state = HNState::CategoryList;
      requestUpdate();
      return;
    }
    const HNStory* story = selectedStory();
    const int itemCount = (story && !story->url.empty()) ? 3 : 1;
    if (mappedInput.wasReleased(Button::Up)) {
      detailMenuIndex = (detailMenuIndex - 1 + itemCount) % itemCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      detailMenuIndex = (detailMenuIndex + 1) % itemCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      if (detailMenuIndex == 0) {
        openComments();
      } else if (detailMenuIndex == 1) {
        openLink();
      } else if (detailMenuIndex == 2) {
        showQrForLink();
      }
    }
    return;
  }

  // CategoryList
  int tab = static_cast<int>(currentTab);
  const int totalRows = static_cast<int>(stories[tab].size()) + 1;  // row 0 = synthetic "Refresh"

  if (mappedInput.wasReleased(Button::Back)) {
    cancelFetchTask();
    finish();
    return;
  } else if (mappedInput.wasReleased(Button::Left)) {
    currentTab = static_cast<HNTab>(ButtonNavigator::previousIndex(tab, HN_TAB_COUNT));
    tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      if (!loadCacheFromSd(tab)) startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Right)) {
    currentTab = static_cast<HNTab>(ButtonNavigator::nextIndex(tab, HN_TAB_COUNT));
    tab = static_cast<int>(currentTab);
    if (!loaded[tab] && errorMessage[tab].empty()) {
      if (!loadCacheFromSd(tab)) startFetch(tab);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Up)) {
    selectedRow[tab] = (selectedRow[tab] - 1 + totalRows) % totalRows;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Down)) {
    selectedRow[tab] = (selectedRow[tab] + 1) % totalRows;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    if (selectedRow[tab] == 0) {
      startFetch(tab);
    } else {
      detailStoryIndex = selectedRow[tab] - 1;
      detailMenuIndex = 0;
      state = HNState::StoryDetail;
      requestUpdate();
    }
  }
}

void HackerNewsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == HNState::StoryDetail) {
    const HNStory* story = selectedStory();
    const std::string title = story ? story->title : "";
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

    const int menuTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int menuBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const bool hasLink = story && !story->url.empty();
    const int itemCount = hasLink ? 3 : 1;

    GUI.drawButtonMenu(
        renderer, Rect{0, menuTop, pageWidth, menuBottom - menuTop}, itemCount, detailMenuIndex,
        [hasLink](int index) -> std::string {
          if (index == 0) return tr(STR_HN_VIEW_COMMENTS);
          if (index == 1) return tr(STR_HN_OPEN_LINK);
          return tr(STR_HN_SEND_TO_PHONE);
        },
        [](int) { return UIIcon::HackerNews; }, 6);

    if (story) {
      char metaBuf[96];
      snprintf(metaBuf, sizeof(metaBuf), tr(STR_HN_STORY_META), story->points, story->author.c_str(),
               story->relativeTime.c_str());
      renderer.drawCenteredText(SMALL_FONT_ID, menuBottom - 20, metaBuf, true, EpdFontFamily::REGULAR);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_HN_TITLE));

    const int tabBarY = metrics.topPadding + metrics.headerHeight;
    std::vector<TabInfo> tabs = {
        {tr(STR_HN_TAB_TOP), currentTab == HNTab::Top},
        {tr(STR_HN_TAB_NEW), currentTab == HNTab::New},
        {tr(STR_HN_TAB_ASK), currentTab == HNTab::Ask},
        {tr(STR_HN_TAB_SHOW), currentTab == HNTab::Show},
    };
    GUI.drawTabBar(renderer, Rect{0, tabBarY, pageWidth, metrics.tabBarHeight}, tabs, true);

    const int contentTop = tabBarY + metrics.tabBarHeight + metrics.verticalSpacing;
    const int tab = static_cast<int>(currentTab);
    const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (!loaded[tab] && stories[tab].empty()) {
      const int textY = contentTop + (listBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      const char* msg = !errorMessage[tab].empty() ? errorMessage[tab].c_str() : tr(STR_HN_LOADING);
      renderer.drawCenteredText(UI_12_FONT_ID, textY, msg);
    } else {
      const auto& tabStories = stories[tab];
      const int totalRows = static_cast<int>(tabStories.size()) + 1;
      GUI.drawList(
          renderer, Rect{0, contentTop, pageWidth, listBottom - contentTop}, totalRows, selectedRow[tab],
          [&tabStories](int i) -> std::string {
            if (i == 0) return tr(STR_HN_REFRESH);
            return tabStories[i - 1].title;
          },
          [&tabStories](int i) -> std::string {
            if (i == 0) return "";
            const auto& s = tabStories[i - 1];
            char buf[96];
            snprintf(buf, sizeof(buf), tr(STR_HN_STORY_META), s.points, s.author.c_str(), s.relativeTime.c_str());
            return buf;
          },
          nullptr,
          [&tabStories](int i) -> std::string {
            if (i == 0) return "";
            char buf[16];
            snprintf(buf, sizeof(buf), tr(STR_HN_COMMENTS_VALUE), tabStories[i - 1].numComments);
            return buf;
          },
          true);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, ButtonArrow::Left,
                        ButtonArrow::Right);
  }

  renderer.displayBuffer();
}
