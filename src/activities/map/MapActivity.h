#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class MapState { Menu, Loading, Viewing, Error };
enum class MapSubMode { Pan, Zoom };
enum class MapPanDirection { North, South, East, West };
enum class MapFetchFailure { None, WifiRequired, Geocode, Download };

// A single 256x256 XYZ tile. rawX is unwrapped (used for screen positioning
// so tiles line up correctly even if the viewport straddles the antimeridian);
// x is wrapped into [0, 2^zoom) as required by the tile URL/cache key.
struct MapTileRef {
  int zoom;
  int rawX;
  int x;
  int y;
};

class MapActivity final : public Activity {
 private:
  MapState state = MapState::Menu;
  MapSubMode subMode = MapSubMode::Pan;

  // Menu
  int selectedMenuIndex = 0;
  static constexpr int MENU_ITEM_COUNT = 3;  // Search, Resume, Server URL

  // Config, persisted at /apps/map/config.json. Template uses the universal
  // XYZ tile URL convention ({z}/{x}/{y}) shared by OSM, MapTiler, Stadia, etc.
  std::string providerUrlTemplate;

  // Current view
  double centerLat = 48.8566;   // Paris, arbitrary default until the user searches
  double centerLon = 2.3522;
  int zoomLevel = DEFAULT_ZOOM;
  bool hasLastView = false;  // true once /apps/map/state.json has been read successfully

  // Viewport in pixels, captured from renderer just before a fetch/render (orientation-aware).
  int viewportWidth = 0;
  int viewportHeight = 0;

  // Background fetch state
  std::string pendingSearchQuery;
  bool isSearchFetch = false;  // true = geocode pendingSearchQuery then fetch; false = fetch current center
  std::string errorMessage;
  MapFetchFailure lastFailure = MapFetchFailure::None;  // set by runBackgroundFetch(); translated to errorMessage on
                                                        // the main thread
  volatile bool pendingUpdate = false;
  bool backgroundFetchSuccess = false;
  void* fetchTaskHandle = nullptr;
  bool wifiWasUsed = false;

  static constexpr int MIN_ZOOM = 3;
  static constexpr int MAX_ZOOM = 18;
  static constexpr int DEFAULT_ZOOM = 12;

  void loadConfig();
  void saveConfig();
  void loadLastView();
  void saveLastView() const;

  std::vector<MapTileRef> tilesForViewport() const;
  std::string buildTileUrl(int zoom, int x, int y) const;
  std::string cachePathForTile(int zoom, int x, int y) const;
  static std::string buildGeocodeUrl(const std::string& query);
  static bool geocode(const std::string& query, double& outLat, double& outLon);
  bool downloadTile(int zoom, int x, int y, const std::string& finalPath) const;
  // Mirrors RssActivity/FootballActivity's debug.log: lets a failure be diagnosed
  // from the SD card's Files page when there's no serial connection available.
  static void appendDebugLog(const std::string& line);

  void captureViewport();
  void startViewFetch();
  void startSearchFetch(const std::string& query);
  void openSearchKeyboard();
  void openServerUrlKeyboard();

  void pan(MapPanDirection direction);
  void zoomIn();
  void zoomOut();

  void renderMenu() const;
  void renderLoading() const;
  void renderViewing() const;
  void renderError() const;

 public:
  void runBackgroundFetch();  // called from the FreeRTOS task trampoline

  explicit MapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Map", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
