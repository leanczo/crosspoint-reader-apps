#include "OnThisDayActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/util/DownloadWatchdog.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"

namespace {

// Short per-attempt timeout for these JSON list fetches (paired with the 3x
// retry loop in runBackgroundFetch()), matching the convention already used
// by Football/HackerNews for this same class of request.
constexpr uint32_t kListFetchTimeoutMs = 10000;

const char* categoryKey(int category) {
  static const char* names[OTD_CATEGORY_COUNT] = {"events", "births", "deaths"};
  return names[category];
}

// This project's Language enum values are its own internal i18n codes (see
// lib/I18n/translations/*.yaml's _language_code), not all of which are real
// Wikipedia subdomains -- e.g. "CAV" (Valencian) isn't a distinct Wikipedia
// edition, and this project's "SI" is Slovenian while Wikipedia/ISO 639-1
// uses "sl". Best-effort mapping; anything wrong here is still caught at
// runtime by the language-fallback probe in runBackgroundFetch().
const char* wikiLangCode(Language lang) {
  switch (lang) {
    case Language::EN: return "en";
    case Language::ES: return "es";
    case Language::FR: return "fr";
    case Language::DE: return "de";
    case Language::CS: return "cs";
    case Language::PT: return "pt";
    case Language::RU: return "ru";
    case Language::SV: return "sv";
    case Language::RO: return "ro";
    case Language::CA: return "ca";
    case Language::UK: return "uk";
    case Language::BE: return "be";
    case Language::IT: return "it";
    case Language::PL: return "pl";
    case Language::FI: return "fi";
    case Language::DA: return "da";
    case Language::NL: return "nl";
    case Language::TR: return "tr";
    case Language::KK: return "kk";
    case Language::HU: return "hu";
    case Language::LT: return "lt";
    case Language::SI: return "sl";
    case Language::CAV: return "ca";
    default: return "en";
  }
}

constexpr StrId kMonthKeys[12] = {
    StrId::STR_MONTH_JANUARY, StrId::STR_MONTH_FEBRUARY, StrId::STR_MONTH_MARCH,     StrId::STR_MONTH_APRIL,
    StrId::STR_MONTH_MAY,     StrId::STR_MONTH_JUNE,     StrId::STR_MONTH_JULY,      StrId::STR_MONTH_AUGUST,
    StrId::STR_MONTH_SEPTEMBER, StrId::STR_MONTH_OCTOBER, StrId::STR_MONTH_NOVEMBER, StrId::STR_MONTH_DECEMBER};

void otdFetchTaskFunc(void* param) {
  auto* activity = static_cast<OnThisDayActivity*>(param);
  activity->runBackgroundFetch();
  vTaskDelete(nullptr);
}

}  // namespace

std::string OnThisDayActivity::cachePath(int category) const {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d-%02d", viewedDate.month, viewedDate.day);
  return "/apps/onthisday/" + activeLangCode + "_" + buf + "_" + categoryKey(category) + ".json";
}

std::string OnThisDayActivity::tmpPath(int category) const { return cachePath(category) + ".tmp"; }

std::string OnThisDayActivity::apiUrl(int category) const {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d/%02d", viewedDate.month, viewedDate.day);
  return "https://" + activeLangCode + ".wikipedia.org/api/rest_v1/feed/onthisday/" + categoryKey(category) + "/" +
        buf;
}

void OnThisDayActivity::appendDebugLog(const std::string& line) {
  String existing = Storage.readFile("/apps/onthisday/debug.log");
  String entry = existing + String(millis()) + "ms: " + String(line.c_str()) + "\n";
  constexpr size_t MAX_LOG_CHARS = 4000;
  if (entry.length() > MAX_LOG_CHARS) {
    entry = entry.substring(entry.length() - MAX_LOG_CHARS);
  }
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/onthisday");
  Storage.writeFile("/apps/onthisday/debug.log", entry);
}

// Cooperative cancel + bounded wait, same pattern as Football/HackerNews.
void OnThisDayActivity::cancelFetchTask() {
  if (fetchTaskHandle != nullptr) {
    if (fetchingCategory >= 0 && fetchingCategory < OTD_CATEGORY_COUNT) {
      refreshing[fetchingCategory] = false;
    }
    cancelFetch = true;
    int waitCount = 0;
    while (fetchTaskHandle != nullptr && waitCount < 1200) {
      delay(10);
      waitCount++;
    }
    if (fetchTaskHandle != nullptr) {
      LOG_ERR("OTD", "Task failed to exit gracefully, forcing vTaskDelete!");
      appendDebugLog("Task failed to exit gracefully, forcing vTaskDelete!");
      TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
      fetchTaskHandle = nullptr;
      vTaskDelete(tempHandle);
    }
  }
  pendingUpdate = false;
}

void OnThisDayActivity::startFetch(int category) {
  cancelFetchTask();
  fetchingCategory = category;
  cancelFetch = false;
  refreshing[category] = true;
  refreshFailed[category] = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/onthisday");

  // Free every resident category's vector before the TLS handshake: mbedTLS
  // needs a contiguous ~32KB (16KB in + 16KB out) buffer on this PSRAM-less
  // chip, and any category's list sitting in RAM is enough to fragment that
  // away. The other two categories just get marked unloaded; the existing
  // tab-switch guard in loop() lazily reloads them from their own SD cache
  // (or re-fetches) the next time the user tabs back to one.
  for (int c = 0; c < OTD_CATEGORY_COUNT; c++) {
    if (c == category) continue;
    if (!entries[c].empty()) {
      entries[c].clear();
      entries[c].shrink_to_fit();
    }
    loaded[c] = false;
  }
  entries[category].clear();
  entries[category].shrink_to_fit();
  state = OnThisDayState::Loading;

  ensureWifiConnected(
      [this]() {
        backgroundFetchSuccess = false;
        xTaskCreate(otdFetchTaskFunc, "otd_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
      },
      [this]() {
        appendDebugLog("WiFi connect cancelled/failed for category " + std::to_string(fetchingCategory));
        refreshing[fetchingCategory] = false;
        // startFetch() already cleared this category's vector above; restore
        // it from disk since the fetch never actually started.
        loadCacheFromSd(fetchingCategory);
        if (!loaded[fetchingCategory]) {
          errorMessage[fetchingCategory] = tr(STR_OTD_WIFI_REQUIRED);
        } else {
          refreshFailed[fetchingCategory] = true;
        }
        state = OnThisDayState::List;
        requestUpdate();
      });
  requestUpdate();
}

void OnThisDayActivity::runBackgroundFetch() {
  DownloadWatchdog::start(60000);
  WifiConnectHelper::waitForTimeSync();

  bool success = false;

  // One cheap, no-retry probe against the current language, run once per
  // activity lifetime. A 404 means this language subdomain doesn't carry
  // onthisday data at all -- a permanent condition, not a transient network
  // blip -- so it must not consume the 3x retry budget below, which exists
  // for flaky connections against a language that *is* supported.
  if (!langFallbackProbed && !cancelFetch) {
    langFallbackProbed = true;
    std::string errDetail;
    auto probe = HttpDownloader::downloadToFile(apiUrl(fetchingCategory), tmpPath(fetchingCategory), nullptr,
                                                &cancelFetch, "", "", nullptr, nullptr, &errDetail,
                                                kListFetchTimeoutMs);
    if (probe == HttpDownloader::OK) {
      success = true;
    } else if (errDetail.find("404") != std::string::npos && activeLangCode != "en") {
      appendDebugLog("Language '" + activeLangCode + "' unsupported by onthisday API (404), falling back to 'en'");
      activeLangCode = "en";
    }
  }

  int retries = success ? 0 : 3;
  int attempt = 0;
  while (!success && retries > 0 && !DownloadWatchdog::gotTimeout && !cancelFetch) {
    attempt++;
    std::string errDetail;
    const auto result = HttpDownloader::downloadToFile(apiUrl(fetchingCategory), tmpPath(fetchingCategory), nullptr,
                                                        &cancelFetch, "", "", nullptr, nullptr, &errDetail,
                                                        kListFetchTimeoutMs);
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

bool OnThisDayActivity::loadCacheFromSd(int category) {
  HalFile file;
  if (!Storage.openFileForRead("OTD", cachePath(category).c_str(), file)) {
    return false;
  }
  parseAndStore(category, file);
  return loaded[category];
}

void OnThisDayActivity::parseAndStore(int category, HalFile& file) {
  // Feed the open file straight to ArduinoJson so the ~700-800KB raw
  // response streams from SD a chunk at a time; only the small *filtered*
  // result tree is kept in RAM. Same pattern as FootballActivity::parseAndStore.
  struct HalFileJsonReader {
    HalFile& f;
    int read() { return f.read(); }
    size_t readBytes(char* buffer, size_t length) {
      const int n = f.read(buffer, length);
      return n < 0 ? 0 : static_cast<size_t>(n);
    }
  } reader{file};

  char heapBuf[112];
  snprintf(heapBuf, sizeof(heapBuf), "Free heap before parse (category %d, %u bytes JSON): %u", category,
           (unsigned)file.size(), (unsigned)ESP.getFreeHeap());
  LOG_INF("OTD", "%s", heapBuf);
  appendDebugLog(heapBuf);

  const char* key = categoryKey(category);
  JsonDocument filter;
  filter[key][0]["text"] = true;
  filter[key][0]["year"] = true;
  filter[key][0]["pages"][0]["content_urls"]["desktop"]["page"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, reader, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(10));
  if (err) {
    appendDebugLog("JSON parse failed for category " + std::to_string(category) + ": " + err.c_str());
    errorMessage[category] = tr(STR_OTD_NO_DATA);
    return;
  }

  // MAX_RESULT_ROWS only bounds the copy below -- the filtered JsonDocument
  // above still holds every filtered entry from the raw response (355 for
  // the heaviest case measured live, es/births). Known limitation, same
  // shape as Football's existing MAX_RESULT_ROWS cap; if this proves too
  // tight on-device, the mitigation is dropping the pages/QR-link sub-filter.
  constexpr size_t MAX_RESULT_ROWS = 25;
  std::vector<OnThisDayEntry> newEntries;
  JsonArray items = doc[key];
  newEntries.reserve(std::min(items.size(), MAX_RESULT_ROWS));
  for (JsonObject item : items) {
    if (newEntries.size() >= MAX_RESULT_ROWS) break;
    std::string text = item["text"] | "";
    if (text.empty()) continue;
    OnThisDayEntry e;
    e.year = item["year"] | 0;
    e.text = std::move(text);
    JsonArray pages = item["pages"];
    if (pages.size() > 0) {
      JsonObject p0 = pages[0];
      e.pageUrl = p0["content_urls"]["desktop"]["page"] | "";
    }
    newEntries.push_back(std::move(e));
  }

  if (newEntries.empty()) {
    errorMessage[category] = tr(STR_OTD_NO_DATA);
    return;
  }

  entries[category] = std::move(newEntries);
  selectedRow[category] = 0;
  scrollOffset[category] = 0;
  loaded[category] = true;
  errorMessage[category].clear();
}

void OnThisDayActivity::changeDate(int deltaDays) {
  const int64_t days = DateMath::daysSinceEpoch(viewedDate) + deltaDays;
  viewedDate = DateMath::civilFromDays(days);  // year may now differ; never displayed/sent to the API

  cancelFetchTask();
  for (int c = 0; c < OTD_CATEGORY_COUNT; c++) {
    entries[c].clear();
    entries[c].shrink_to_fit();
    loaded[c] = false;
    errorMessage[c].clear();
    refreshFailed[c] = false;
    selectedRow[c] = 0;
    scrollOffset[c] = 0;
  }
  int cat = static_cast<int>(currentCategory);
  if (!loadCacheFromSd(cat)) startFetch(cat);
  requestUpdate();
}

void OnThisDayActivity::showQrForSelected() {
  int cat = static_cast<int>(currentCategory);
  int idx = selectedRow[cat] - 3;
  if (idx < 0 || idx >= static_cast<int>(entries[cat].size())) return;
  const std::string url = entries[cat][idx].pageUrl;
  if (url.empty()) return;
  startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, url),
                         [this](const ActivityResult&) { requestUpdate(); });
}

std::string OnThisDayActivity::formattedHeaderDate() const {
  std::string monthName = I18N.get(kMonthKeys[viewedDate.month - 1]);
  std::string dayStr = std::to_string(viewedDate.day);

  std::string result = tr(STR_OTD_DATE_FORMAT);
  size_t pos = result.find("{MONTH}");
  if (pos != std::string::npos) result.replace(pos, 7, monthName);
  pos = result.find("{DAY}");
  if (pos != std::string::npos) result.replace(pos, 5, dayStr);
  return result;
}

void OnThisDayActivity::onEnter() {
  Activity::onEnter();
  pendingUpdate = false;
  fetchTaskHandle = nullptr;
  langFallbackProbed = false;
  activeLangCode = wikiLangCode(I18N.getLanguage());

  time_t now = time(nullptr);
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  viewedDate.year = static_cast<int16_t>(tmNow.tm_year + 1900);
  viewedDate.month = static_cast<uint8_t>(tmNow.tm_mon + 1);
  viewedDate.day = static_cast<uint8_t>(tmNow.tm_mday);

  int cat = static_cast<int>(currentCategory);
  if (!loadCacheFromSd(cat)) {
    startFetch(cat);
  }
  requestUpdate();
}

void OnThisDayActivity::onExit() {
  Activity::onExit();
  cancelFetchTask();
}

void OnThisDayActivity::loop() {
  if (pendingUpdate) {
    pendingUpdate = false;
    fetchTaskHandle = nullptr;
    int cat = fetchingCategory;
    refreshing[cat] = false;
    if (backgroundFetchSuccess) {
      Storage.remove(cachePath(cat).c_str());
      Storage.rename(tmpPath(cat).c_str(), cachePath(cat).c_str());
    }
    // startFetch() cleared this category's vector before the fetch started,
    // so the reload must happen unconditionally -- on failure this is the
    // only way to get the old (still-good, untouched-on-disk) data back.
    loadCacheFromSd(cat);
    if (!loaded[cat]) {
      if (errorMessage[cat].empty()) errorMessage[cat] = tr(STR_OTD_NO_DATA);
    } else if (!backgroundFetchSuccess) {
      refreshFailed[cat] = true;
    }
    if (state == OnThisDayState::Loading) state = OnThisDayState::List;
    requestUpdate();
  }

  using Button = MappedInputManager::Button;

  if (state == OnThisDayState::Loading) {
    if (mappedInput.wasReleased(Button::Back)) {
      // cancelFetchTask() already clears refreshing[currentCategory] for us.
      cancelFetchTask();
      loadCacheFromSd(static_cast<int>(currentCategory));
      state = OnThisDayState::List;
      requestUpdate();
    }
    return;
  }

  int cat = static_cast<int>(currentCategory);
  const int totalRows = 3 + static_cast<int>(entries[cat].size());  // 0=Refresh,1=PrevDay,2=NextDay

  if (mappedInput.wasReleased(Button::Back)) {
    cancelFetchTask();
    finish();
    return;
  } else if (mappedInput.wasReleased(Button::Left)) {
    currentCategory = static_cast<OnThisDayCategory>(ButtonNavigator::previousIndex(cat, OTD_CATEGORY_COUNT));
    cat = static_cast<int>(currentCategory);
    if (!loaded[cat] && errorMessage[cat].empty()) {
      if (!loadCacheFromSd(cat)) startFetch(cat);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Right)) {
    currentCategory = static_cast<OnThisDayCategory>(ButtonNavigator::nextIndex(cat, OTD_CATEGORY_COUNT));
    cat = static_cast<int>(currentCategory);
    if (!loaded[cat] && errorMessage[cat].empty()) {
      if (!loadCacheFromSd(cat)) startFetch(cat);
    }
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Up)) {
    if (selectedRow[cat] > 0) {
      selectedRow[cat]--;
      // Scrolling into view past the top is a simple snap, valid regardless
      // of card height; growing past the bottom depends on how many cards
      // actually fit at their natural (variable) height, which only
      // render() knows -- see its scroll-adjustment step.
      const int entryIdx = selectedRow[cat] - 3;
      if (entryIdx >= 0 && entryIdx < scrollOffset[cat]) scrollOffset[cat] = entryIdx;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(Button::Down)) {
    if (selectedRow[cat] < totalRows - 1) {
      selectedRow[cat]++;
      requestUpdate();
    }
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    if (selectedRow[cat] == 0) {
      startFetch(cat);
    } else if (selectedRow[cat] == 1) {
      changeDate(-1);
    } else if (selectedRow[cat] == 2) {
      changeDate(1);
    } else {
      showQrForSelected();
    }
  }
}

void OnThisDayActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const std::string dateStr = formattedHeaderDate();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OTD_TITLE),
                dateStr.c_str());

  const int tabBarY = metrics.topPadding + metrics.headerHeight;
  const int cat = static_cast<int>(currentCategory);
  std::vector<TabInfo> tabs = {
      {tr(STR_OTD_TAB_EVENTS), currentCategory == OnThisDayCategory::Events},
      {tr(STR_OTD_TAB_BIRTHS), currentCategory == OnThisDayCategory::Births},
      {tr(STR_OTD_TAB_DEATHS), currentCategory == OnThisDayCategory::Deaths},
  };
  GUI.drawTabBar(renderer, Rect{0, tabBarY, pageWidth, metrics.tabBarHeight}, tabs, true);

  const int contentTop = tabBarY + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (state == OnThisDayState::Loading) {
    const int textY = contentTop + (contentBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, loaded[cat] ? tr(STR_OTD_REFRESHING) : tr(STR_OTD_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int sideX = metrics.contentSidePadding;
  const int rowWidth = pageWidth - 2 * sideX;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  // 3 synthetic rows: Refresh / Previous Day / Next Day, same convention
  // HackerNews uses for its single "Refresh" row 0, extended to cover date
  // navigation too since no dedicated button pair is free for it.
  constexpr int kSyntheticRowHeight = 30;
  const char* syntheticLabels[3] = {tr(STR_OTD_REFRESH), tr(STR_OTD_PREV_DAY), tr(STR_OTD_NEXT_DAY)};
  int y = contentTop;
  for (int i = 0; i < 3; i++) {
    const bool isSelected = (selectedRow[cat] == i);
    if (isSelected) renderer.fillRect(sideX, y, rowWidth, kSyntheticRowHeight);
    renderer.drawText(UI_10_FONT_ID, sideX + 8, y + 6, syntheticLabels[i], !isSelected);
    y += kSyntheticRowHeight;
  }
  y += 6;

  if (entries[cat].empty()) {
    if (!errorMessage[cat].empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, y + 20, errorMessage[cat].c_str(), true, EpdFontFamily::BOLD);
    }
  } else {
    // Cards are sized to their own wrapped-text line count (year row + up to
    // 3 body lines + padding) rather than an equal division of the content
    // area -- most entries wrap to 1-2 lines, so a fixed tall slot per card
    // wasted more than half the screen. Computing wrappedText per candidate
    // card is cheap: at most 25 entries exist, and e-ink only re-renders on
    // user action, not per frame.
    constexpr int kCardPadding = 18;   // top+bottom padding inside a card
    constexpr int kCardGap = 8;        // vertical gap between cards
    constexpr int kMaxBodyLines = 3;
    auto cardHeightFor = [&](const OnThisDayEntry& e) {
      auto lines = renderer.wrappedText(UI_10_FONT_ID, e.text.c_str(), rowWidth - 24, kMaxBodyLines, EpdFontFamily::REGULAR);
      return lineHeight + 4 + static_cast<int>(lines.size()) * lineHeight + kCardPadding + kCardGap;
    };

    // If the selected entry lies below the currently visible window, grow
    // scrollOffset until it fits; scrolling upward (selectedIdx < scrollOffset)
    // is already snapped by loop()'s Up handler.
    const int selectedEntryIdx = selectedRow[cat] - 3;
    if (selectedEntryIdx >= scrollOffset[cat] && selectedEntryIdx < static_cast<int>(entries[cat].size())) {
      while (true) {
        int yy = y;
        int lastVisible = scrollOffset[cat] - 1;
        for (int idx = scrollOffset[cat]; idx < static_cast<int>(entries[cat].size()); idx++) {
          const int h = cardHeightFor(entries[cat][idx]);
          if (yy + h > contentBottom) break;
          yy += h;
          lastVisible = idx;
        }
        if (selectedEntryIdx <= lastVisible || scrollOffset[cat] >= selectedEntryIdx) break;
        scrollOffset[cat]++;
      }
    }

    int drawY = y;
    for (int idx = scrollOffset[cat]; idx < static_cast<int>(entries[cat].size()); idx++) {
      const auto& e = entries[cat][idx];
      auto lines = renderer.wrappedText(UI_10_FONT_ID, e.text.c_str(), rowWidth - 24, kMaxBodyLines, EpdFontFamily::REGULAR);
      const int cardH = lineHeight + 4 + static_cast<int>(lines.size()) * lineHeight + kCardPadding + kCardGap;
      if (drawY + cardH > contentBottom) break;

      const bool isSelected = (selectedRow[cat] == idx + 3);
      renderer.drawRoundedRect(sideX, drawY, rowWidth, cardH - kCardGap, isSelected ? 3 : 1, 8, true);

      char yearBuf[16];
      if (e.year < 0) {
        snprintf(yearBuf, sizeof(yearBuf), tr(STR_OTD_YEAR_BC), -e.year);
      } else {
        snprintf(yearBuf, sizeof(yearBuf), "%d", e.year);
      }
      int textY = drawY + kCardPadding / 2;
      renderer.drawText(UI_10_FONT_ID, sideX + 12, textY, yearBuf, true, EpdFontFamily::BOLD);
      textY += lineHeight + 4;

      for (const auto& line : lines) {
        renderer.drawText(UI_10_FONT_ID, sideX + 12, textY, line.c_str(), true, EpdFontFamily::REGULAR);
        textY += lineHeight;
      }
      drawY += cardH;
    }
  }

  if (refreshFailed[cat]) {
    GUI.drawPopup(renderer, tr(STR_OTD_REFRESH_FAILED), false);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, ButtonArrow::Left,
                      ButtonArrow::Right);

  renderer.displayBuffer();
}
