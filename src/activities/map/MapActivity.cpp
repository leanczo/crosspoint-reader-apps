#include "MapActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "MappedInputManager.h"
#include "activities/util/DownloadWatchdog.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

constexpr char CONFIG_DIR[] = "/apps/map";
constexpr char CACHE_DIR[] = "/apps/map/cache";
constexpr char CONFIG_PATH[] = "/apps/map/config.json";
constexpr char STATE_PATH[] = "/apps/map/state.json";
constexpr char DEBUG_LOG_PATH[] = "/apps/map/debug.log";
// The official OSM tile server: free, no API key, no signup, and (unlike the
// "staticmap" arbitrary-size generators, most of which have gone offline)
// actively maintained. Follows the universal XYZ tile URL convention shared
// by essentially every raster tile provider (MapTiler, Stadia, self-hosted...),
// so users can point this at their own provider by editing the URL template.
constexpr char DEFAULT_PROVIDER_TEMPLATE[] = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
constexpr char GEOCODE_URL_BASE[] = "https://nominatim.openstreetmap.org/search?format=json&limit=1&q=";
constexpr double TILE_SIZE = 256.0;

void mapFetchTaskFunc(void* param) {
  auto* activity = static_cast<MapActivity*>(param);
  activity->runBackgroundFetch();
  vTaskDelete(nullptr);
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.length(), to);
    pos += to.length();
  }
  return s;
}

std::string urlEncode(const std::string& value) {
  std::string escaped;
  for (char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      escaped += c;
    } else if (c == ' ') {
      escaped += "+";
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
      escaped += hex;
    }
  }
  return escaped;
}

// FNV-1a: cheap, deterministic, good enough to namespace the tile cache by
// provider (so switching providers can't serve stale tiles from another
// provider's style) without pulling in a real hash library.
uint32_t fnv1aHash(const std::string& s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

std::string formatCoord(double v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.5f", v);
  return buf;
}

// Standard Web Mercator "slippy map" tile math (see OSM wiki "Slippy map
// tilenames"): the whole world at a given zoom is (256 * 2^zoom) pixels
// square, longitude maps linearly, latitude maps through the Mercator
// projection (asinh/sinh), and both share this common tiles-per-axis count.
double tilesPerAxis(int zoom) { return std::pow(2.0, zoom); }

double lonToGlobalPixelX(double lon, int zoom) { return (lon + 180.0) / 360.0 * TILE_SIZE * tilesPerAxis(zoom); }

double latToGlobalPixelY(double lat, int zoom) {
  const double latRad = lat * M_PI / 180.0;
  return (1.0 - std::asinh(std::tan(latRad)) / M_PI) / 2.0 * TILE_SIZE * tilesPerAxis(zoom);
}

double globalPixelXToLon(double px, int zoom) { return px / (TILE_SIZE * tilesPerAxis(zoom)) * 360.0 - 180.0; }

double globalPixelYToLat(double py, int zoom) {
  const double n = M_PI - 2.0 * M_PI * py / (TILE_SIZE * tilesPerAxis(zoom));
  return 180.0 / M_PI * std::atan(std::sinh(n));
}

// Greedy word-wrap: drawCenteredText() draws a single unwrapped line, so a
// message longer than the screen width (easy to hit in portrait orientation,
// 480px wide) gets silently clipped instead of shown in full.
std::vector<std::string> wrapText(const GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth) {
  std::vector<std::string> lines;
  std::string line;
  std::string word;
  auto flushWord = [&]() {
    if (word.empty()) return;
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (line.empty() || renderer.getTextWidth(fontId, candidate.c_str()) <= maxWidth) {
      line = candidate;
    } else {
      lines.push_back(line);
      line = word;
    }
    word.clear();
  };
  for (char c : text) {
    if (c == ' ') {
      flushWord();
    } else {
      word += c;
    }
  }
  flushWord();
  if (!line.empty()) lines.push_back(line);
  return lines;
}

}  // namespace

void MapActivity::appendDebugLog(const std::string& line) {
  String existing = Storage.readFile(DEBUG_LOG_PATH);
  String entry = existing + String(millis()) + "ms: " + String(line.c_str()) + "\n";
  constexpr size_t MAX_LOG_CHARS = 4000;
  if (entry.length() > MAX_LOG_CHARS) {
    entry = entry.substring(entry.length() - MAX_LOG_CHARS);
  }
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists(CONFIG_DIR);
  Storage.writeFile(DEBUG_LOG_PATH, entry);
}

void MapActivity::loadConfig() {
  providerUrlTemplate = DEFAULT_PROVIDER_TEMPLATE;
  String json = Storage.readFile(CONFIG_PATH);
  if (json.isEmpty()) return;
  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) return;
  std::string url = doc["providerUrlTemplate"] | std::string(DEFAULT_PROVIDER_TEMPLATE);
  if (!url.empty()) providerUrlTemplate = url;
}

void MapActivity::saveConfig() {
  Storage.ensureDirectoryExists(CONFIG_DIR);
  JsonDocument doc;
  doc["providerUrlTemplate"] = providerUrlTemplate;
  String json;
  serializeJson(doc, json);
  Storage.writeFile(CONFIG_PATH, json);
}

void MapActivity::loadLastView() {
  hasLastView = false;
  String json = Storage.readFile(STATE_PATH);
  if (json.isEmpty()) return;
  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) return;
  if (doc["lat"].isNull() || doc["lon"].isNull()) return;
  centerLat = doc["lat"] | centerLat;
  centerLon = doc["lon"] | centerLon;
  zoomLevel = doc["zoom"] | DEFAULT_ZOOM;
  if (zoomLevel < MIN_ZOOM) zoomLevel = MIN_ZOOM;
  if (zoomLevel > MAX_ZOOM) zoomLevel = MAX_ZOOM;
  hasLastView = true;
}

void MapActivity::saveLastView() const {
  if (!hasLastView) return;
  Storage.ensureDirectoryExists(CONFIG_DIR);
  JsonDocument doc;
  doc["lat"] = centerLat;
  doc["lon"] = centerLon;
  doc["zoom"] = zoomLevel;
  String json;
  serializeJson(doc, json);
  Storage.writeFile(STATE_PATH, json);
}

std::vector<MapTileRef> MapActivity::tilesForViewport() const {
  const double cx = lonToGlobalPixelX(centerLon, zoomLevel);
  const double cy = latToGlobalPixelY(centerLat, zoomLevel);
  const double originX = cx - viewportWidth / 2.0;
  const double originY = cy - viewportHeight / 2.0;

  const int firstTileX = static_cast<int>(std::floor(originX / TILE_SIZE));
  const int firstTileY = static_cast<int>(std::floor(originY / TILE_SIZE));
  const int lastTileX = static_cast<int>(std::floor((originX + viewportWidth) / TILE_SIZE));
  const int lastTileY = static_cast<int>(std::floor((originY + viewportHeight) / TILE_SIZE));

  const int n = static_cast<int>(tilesPerAxis(zoomLevel));
  std::vector<MapTileRef> tiles;
  tiles.reserve(static_cast<size_t>((lastTileX - firstTileX + 1) * (lastTileY - firstTileY + 1)));
  for (int ty = firstTileY; ty <= lastTileY; ty++) {
    if (ty < 0 || ty >= n) continue;  // no vertical wraparound (poles)
    for (int tx = firstTileX; tx <= lastTileX; tx++) {
      const int wrappedX = ((tx % n) + n) % n;  // longitude wraps around the world
      tiles.push_back(MapTileRef{zoomLevel, tx, wrappedX, ty});
    }
  }
  return tiles;
}

std::string MapActivity::buildTileUrl(int zoom, int x, int y) const {
  std::string url = providerUrlTemplate;
  url = replaceAll(url, "{z}", std::to_string(zoom));
  url = replaceAll(url, "{x}", std::to_string(x));
  url = replaceAll(url, "{y}", std::to_string(y));
  return url;
}

std::string MapActivity::cachePathForTile(int zoom, int x, int y) const {
  char hex[16];
  snprintf(hex, sizeof(hex), "%08x", static_cast<unsigned>(fnv1aHash(providerUrlTemplate)));
  char buf[80];
  snprintf(buf, sizeof(buf), "%s/%s/z%d_%d_%d.png", CACHE_DIR, hex, zoom, x, y);
  return buf;
}

std::string MapActivity::buildGeocodeUrl(const std::string& query) { return std::string(GEOCODE_URL_BASE) + urlEncode(query); }

bool MapActivity::geocode(const std::string& query, double& outLat, double& outLon) {
  const std::string url = buildGeocodeUrl(query);
  appendDebugLog("Geocoding: " + url);
  std::string response;
  int retries = 3;
  while (retries > 0 && !DownloadWatchdog::gotTimeout) {
    response.clear();
    if (HttpDownloader::fetchUrl(url, response) && !response.empty()) break;
    retries--;
    if (retries > 0 && !DownloadWatchdog::gotTimeout) delay(1000);
  }
  if (response.empty()) {
    appendDebugLog("Geocode failed: empty response (network/HTTP error, see LOG_ERR HTTP tag)");
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    appendDebugLog("Geocode failed: JSON parse error. Response head: " + response.substr(0, 120));
    return false;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) {
    appendDebugLog("Geocode failed: no results for query \"" + query + "\"");
    return false;
  }
  JsonObject first = arr[0];
  std::string latStr = first["lat"] | "";
  std::string lonStr = first["lon"] | "";
  if (latStr.empty() || lonStr.empty()) {
    appendDebugLog("Geocode failed: result missing lat/lon fields");
    return false;
  }
  outLat = atof(latStr.c_str());
  outLon = atof(lonStr.c_str());
  appendDebugLog("Geocode OK: " + formatCoord(outLat) + "," + formatCoord(outLon));
  return true;
}

bool MapActivity::downloadTile(int zoom, int x, int y, const std::string& finalPath) const {
  const std::string url = buildTileUrl(zoom, x, y);
  const std::string tmpPath = std::string(CACHE_DIR) + "/download.tmp.png";
  Storage.ensureDirectoryExists(CACHE_DIR);
  // finalPath is CACHE_DIR/<providerHash>/z_x_y.png -- ensure the provider subdir exists too.
  const size_t lastSlash = finalPath.find_last_of('/');
  if (lastSlash != std::string::npos) Storage.ensureDirectoryExists(finalPath.substr(0, lastSlash).c_str());

  bool ok = false;
  int retries = 3;
  while (retries > 0 && !DownloadWatchdog::gotTimeout) {
    std::string errDetail;
    const auto result =
        HttpDownloader::downloadToFile(url, tmpPath, nullptr, nullptr, "", "", nullptr, nullptr, &errDetail);
    if (result == HttpDownloader::OK) {
      ok = true;
      break;
    }
    LOG_ERR("MAP", "tile download failed: %s", errDetail.c_str());
    appendDebugLog("Download failed (" + std::to_string(retries - 1) + " retries left): " + url + " -> " + errDetail);
    retries--;
    if (retries > 0 && !DownloadWatchdog::gotTimeout) delay(1000);
  }
  if (!ok) {
    if (DownloadWatchdog::gotTimeout) appendDebugLog("Download watchdog timeout - WiFi force-disconnected");
    return false;
  }

  // Guard against a provider returning HTTP 200 with a non-image error page
  // (common for an out-of-range zoom/coordinate) permanently poisoning the cache.
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(tmpPath);
  ImageDimensions dims{};
  if (!decoder || !decoder->getDimensions(tmpPath, dims)) {
    LOG_ERR("MAP", "downloaded tile is not a decodable image");
    appendDebugLog("Downloaded file is not a decodable PNG/JPEG: " + url);
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(finalPath.c_str())) Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

void MapActivity::captureViewport() {
  int viewTop, viewRight, viewBottom, viewLeft;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int STATUS_LINE_HEIGHT = 24;
  viewportWidth = renderer.getScreenWidth() - viewLeft - viewRight;
  viewportHeight = renderer.getScreenHeight() - viewTop - viewBottom - metrics.buttonHintsHeight - STATUS_LINE_HEIGHT;
  if (viewportWidth < 64) viewportWidth = 64;
  if (viewportHeight < 64) viewportHeight = 64;
}

void MapActivity::startViewFetch() {
  captureViewport();
  isSearchFetch = false;

  const auto tiles = tilesForViewport();
  bool allCached = !tiles.empty();
  for (const auto& t : tiles) {
    if (!Storage.exists(cachePathForTile(t.zoom, t.x, t.y).c_str())) {
      allCached = false;
      break;
    }
  }
  if (allCached) {
    state = MapState::Viewing;
    hasLastView = true;
    requestUpdate();
    return;
  }

  state = MapState::Loading;
  requestUpdate();
  ensureWifiConnected(
      [this]() {
        wifiWasUsed = true;
        backgroundFetchSuccess = false;
        xTaskCreate(mapFetchTaskFunc, "map_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
      },
      [this]() {
        lastFailure = MapFetchFailure::WifiRequired;
        errorMessage = tr(STR_MAP_WIFI_REQUIRED);
        appendDebugLog("Wi-Fi connect cancelled/failed for view fetch");
        state = MapState::Error;
        requestUpdate();
      });
}

void MapActivity::startSearchFetch(const std::string& query) {
  pendingSearchQuery = query;
  isSearchFetch = true;
  captureViewport();
  state = MapState::Loading;
  requestUpdate();
  ensureWifiConnected(
      [this]() {
        wifiWasUsed = true;
        backgroundFetchSuccess = false;
        xTaskCreate(mapFetchTaskFunc, "map_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
      },
      [this]() {
        lastFailure = MapFetchFailure::WifiRequired;
        errorMessage = tr(STR_MAP_WIFI_REQUIRED);
        appendDebugLog("Wi-Fi connect cancelled/failed for search fetch");
        state = MapState::Error;
        requestUpdate();
      });
}

void MapActivity::openSearchKeyboard() {
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_MAP_MENU_SEARCH), "", 60);
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
      if (keyboardResult && !keyboardResult->text.empty()) {
        startSearchFetch(keyboardResult->text);
        return;
      }
    }
    requestUpdate();
  });
}

void MapActivity::openServerUrlKeyboard() {
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_MAP_MENU_SERVER_URL),
                                                          providerUrlTemplate, 160, InputType::Url);
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
      if (keyboardResult && !keyboardResult->text.empty()) {
        providerUrlTemplate = keyboardResult->text;
        saveConfig();
      }
    }
    requestUpdate();
  });
}

void MapActivity::pan(MapPanDirection direction) {
  double px = lonToGlobalPixelX(centerLon, zoomLevel);
  double py = latToGlobalPixelY(centerLat, zoomLevel);
  constexpr double PAN_OVERLAP_FRACTION = 0.6;  // leave ~40% of the view in common for context
  const double xStep = viewportWidth * PAN_OVERLAP_FRACTION;
  const double yStep = viewportHeight * PAN_OVERLAP_FRACTION;

  switch (direction) {
    case MapPanDirection::North:
      py -= yStep;
      break;
    case MapPanDirection::South:
      py += yStep;
      break;
    case MapPanDirection::East:
      px += xStep;
      break;
    case MapPanDirection::West:
      px -= xStep;
      break;
  }

  const double worldPixels = TILE_SIZE * tilesPerAxis(zoomLevel);
  py = std::max(0.0, std::min(worldPixels, py));  // clamp vertically, Mercator has no pole coverage
  px = std::fmod(px, worldPixels);                // longitude wraps around the world
  if (px < 0.0) px += worldPixels;

  centerLon = globalPixelXToLon(px, zoomLevel);
  centerLat = globalPixelYToLat(py, zoomLevel);
  startViewFetch();
}

void MapActivity::zoomIn() {
  if (zoomLevel < MAX_ZOOM) {
    zoomLevel++;
    startViewFetch();
  }
}

void MapActivity::zoomOut() {
  if (zoomLevel > MIN_ZOOM) {
    zoomLevel--;
    startViewFetch();
  }
}

void MapActivity::runBackgroundFetch() {
  DownloadWatchdog::start(60000);

  bool ok = true;
  lastFailure = MapFetchFailure::None;
  if (isSearchFetch) {
    ok = geocode(pendingSearchQuery, centerLat, centerLon);
    if (!ok) lastFailure = MapFetchFailure::Geocode;
    if (ok) zoomLevel = DEFAULT_ZOOM;
  }

  if (ok) {
    const auto tiles = tilesForViewport();
    for (const auto& t : tiles) {
      if (DownloadWatchdog::gotTimeout) {
        ok = false;
        break;
      }
      const std::string path = cachePathForTile(t.zoom, t.x, t.y);
      if (Storage.exists(path.c_str())) continue;
      if (!downloadTile(t.zoom, t.x, t.y, path)) {
        ok = false;
        lastFailure = MapFetchFailure::Download;
        break;
      }
    }
  }

  DownloadWatchdog::stop();
  if (DownloadWatchdog::gotTimeout) {
    ok = false;
    lastFailure = MapFetchFailure::Download;
  }

  backgroundFetchSuccess = ok;
  if (ok) hasLastView = true;
  pendingUpdate = true;
}

void MapActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists(CONFIG_DIR);
  loadConfig();
  loadLastView();
  state = MapState::Menu;
  subMode = MapSubMode::Pan;
  selectedMenuIndex = 0;
  pendingUpdate = false;
  fetchTaskHandle = nullptr;
  requestUpdate();
}

void MapActivity::onExit() {
  Activity::onExit();
  saveLastView();
  if (fetchTaskHandle != nullptr) {
    // Killing a task mid-HTTP-request leaks its open TLS/esp_http_client buffers,
    // but leaving it running would touch a dangling `this` after this Activity is
    // deleted -- the crash is worse than the leak, so this mirrors FootballActivity.
    TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
    fetchTaskHandle = nullptr;
    vTaskDelete(tempHandle);
  }
  pendingUpdate = false;
  if (wifiWasUsed) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

void MapActivity::loop() {
  if (pendingUpdate) {
    pendingUpdate = false;
    fetchTaskHandle = nullptr;
    if (backgroundFetchSuccess) {
      state = MapState::Viewing;
      saveLastView();
    } else {
      state = MapState::Error;
      switch (lastFailure) {
        case MapFetchFailure::Geocode:
          errorMessage = tr(STR_MAP_GEOCODE_FAILED);
          break;
        case MapFetchFailure::Download:
          errorMessage = tr(STR_MAP_DOWNLOAD_FAILED);
          break;
        case MapFetchFailure::WifiRequired:
        case MapFetchFailure::None:
        default:
          errorMessage = tr(STR_MAP_NO_CACHE);
          break;
      }
    }
    requestUpdate();
  }

  if (state == MapState::Menu) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedMenuIndex = (selectedMenuIndex - 1 + MENU_ITEM_COUNT) % MENU_ITEM_COUNT;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedMenuIndex = (selectedMenuIndex + 1) % MENU_ITEM_COUNT;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedMenuIndex == 0) {
        openSearchKeyboard();
      } else if (selectedMenuIndex == 1) {
        if (hasLastView) startViewFetch();
      } else if (selectedMenuIndex == 2) {
        openServerUrlKeyboard();
      }
    }
    return;
  }

  if (state == MapState::Viewing) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = MapState::Menu;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      subMode = (subMode == MapSubMode::Pan) ? MapSubMode::Zoom : MapSubMode::Pan;
      requestUpdate();
      return;
    }
    if (subMode == MapSubMode::Pan) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        pan(MapPanDirection::North);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        pan(MapPanDirection::South);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        pan(MapPanDirection::West);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        pan(MapPanDirection::East);
      }
    } else {
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        zoomIn();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        zoomOut();
      }
    }
    return;
  }

  if (state == MapState::Error) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = MapState::Menu;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (isSearchFetch && !pendingSearchQuery.empty()) {
        startSearchFetch(pendingSearchQuery);
      } else {
        startViewFetch();
      }
    }
  }
}

void MapActivity::render(RenderLock&&) {
  switch (state) {
    case MapState::Menu:
      renderMenu();
      break;
    case MapState::Loading:
      renderLoading();
      break;
    case MapState::Viewing:
      renderViewing();
      break;
    case MapState::Error:
      renderError();
      break;
  }
}

void MapActivity::renderMenu() const {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MAP_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const std::string menuLabels[MENU_ITEM_COUNT] = {
      tr(STR_MAP_MENU_SEARCH),
      tr(STR_MAP_MENU_RESUME),
      tr(STR_MAP_MENU_SERVER_URL),
  };
  const bool resumeAvailable = hasLastView;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEM_COUNT, selectedMenuIndex,
      [&menuLabels](int index) { return menuLabels[index]; }, nullptr, nullptr, nullptr, false,
      [resumeAvailable](int index) { return index == 1 && !resumeAvailable; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void MapActivity::renderLoading() const {
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_LOADING));
  renderer.displayBuffer();
}

void MapActivity::renderViewing() const {
  renderer.clearScreen();

  int viewTop, viewRight, viewBottom, viewLeft;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);

  const double cx = lonToGlobalPixelX(centerLon, zoomLevel);
  const double cy = latToGlobalPixelY(centerLat, zoomLevel);
  const double originX = cx - viewportWidth / 2.0;
  const double originY = cy - viewportHeight / 2.0;

  for (const auto& t : tilesForViewport()) {
    const std::string path = cachePathForTile(t.zoom, t.x, t.y);
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(path);
    if (!decoder) continue;

    RenderConfig config;
    config.x = viewLeft + static_cast<int>(std::round(t.rawX * TILE_SIZE - originX));
    config.y = viewTop + static_cast<int>(std::round(t.y * TILE_SIZE - originY));
    config.maxWidth = static_cast<int>(TILE_SIZE);
    config.maxHeight = static_cast<int>(TILE_SIZE);
    config.useGrayscale = true;
    config.useDithering = true;
    config.useExactDimensions = true;
    if (!decoder->decodeToFramebuffer(path, renderer, config)) {
      LOG_ERR("MAP", "failed to decode cached tile: %s", path.c_str());
    }
  }

  char statusBuf[48];
  snprintf(statusBuf, sizeof(statusBuf), "%s - Zoom %d",
           subMode == MapSubMode::Pan ? tr(STR_MAP_PAN_MODE) : tr(STR_MAP_ZOOM_MODE), zoomLevel);
  renderer.drawCenteredText(SMALL_FONT_ID, viewTop + viewportHeight + 4, statusBuf, true);

  const auto labels = (subMode == MapSubMode::Pan)
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_MAP_ZOOM_MODE), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                          : mappedInput.mapLabels(tr(STR_BACK), tr(STR_MAP_PAN_MODE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, subMode == MapSubMode::Pan ? "^" : "+", subMode == MapSubMode::Pan ? "v" : "-");

  renderer.displayBuffer();
}

void MapActivity::renderError() const {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MAP_TITLE));

  const int maxTextWidth = pageWidth - metrics.contentSidePadding * 2;
  const auto lines = wrapText(renderer, UI_12_FONT_ID, errorMessage, maxTextWidth);
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  int textY = pageHeight / 2 - static_cast<int>(lines.size()) * lineHeight / 2;
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_12_FONT_ID, textY, line.c_str(), true);
    textY += lineHeight;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
