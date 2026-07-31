#include "AppRegistry.h"
#include "activities/calculator/CalculatorActivity.h"
#include "activities/chess/ChessActivity.h"
#include "activities/dice/DiceActivity.h"
#include "activities/duckduckgo/DuckDuckGoActivity.h"
#include "activities/formulaone/FormulaOneActivity.h"
#include "activities/home/AppFolderActivity.h"
#include "activities/rss/RssActivity.h"
#include "activities/sudoku/SudokuActivity.h"
#include "activities/tictactoe/TicTacToeActivity.h"
#include "activities/weather/WeatherActivity.h"
#include "activities/wikipedia/WikipediaActivity.h"

// System Activities
#include "I18n.h"
#include "OpdsServerStore.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/settings/OpdsServerListActivity.h"
#include "activities/settings/SettingsActivity.h"

AppRegistry &AppRegistry::getInstance() {
  static AppRegistry instance;
  return instance;
}

AppRegistry::AppRegistry() {
  // Browse Files
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_BROWSE_FILES); }, UIIcon::Folder,
      [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<FileBrowserActivity>(r, i);
      }));

  // Recent Books
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_MENU_RECENT_BOOKS); }, UIIcon::Recent,
      [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<RecentBooksActivity>(r, i);
      }));

  // OPDS Browser (conditionally visible)
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_OPDS_BROWSER); }, UIIcon::Library,
      [](GfxRenderer &r, MappedInputManager &i) -> std::unique_ptr<Activity> {
        const auto &servers = OPDS_STORE.getServers();
        if (servers.size() == 1) {
          return std::make_unique<OpdsBookBrowserActivity>(r, i, servers[0]);
        } else {
          return std::make_unique<OpdsServerListActivity>(r, i, true);
        }
      },
      []() { return OPDS_STORE.hasServers(); }));

  // File Transfer
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_FILE_TRANSFER); }, UIIcon::Transfer,
      [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<CrossPointWebServerActivity>(r, i);
      }));

  // Settings
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_SETTINGS_TITLE); }, UIIcon::Settings,
      [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<SettingsActivity>(r, i);
      }));

  // Games folder
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_FOLDER_GAMES); }, UIIcon::Chess,
      [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<AppFolderActivity>(r, i, AppCategory::Games);
      }));

  // Entertainment folder
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_FOLDER_ENTERTAINMENT); }, UIIcon::Wikipedia,
      [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<AppFolderActivity>(r, i, AppCategory::Entertainment);
      }));

  // Tools folder
  apps.push_back(std::make_unique<App>(
      []() { return tr(STR_FOLDER_TOOLS); }, UIIcon::Calculator,
      [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<AppFolderActivity>(r, i, AppCategory::Tools);
      }));

  // --- Games ---

  // Chess App
  gamesApps.push_back(std::make_unique<App>(
      []() { return tr(STR_CHESS_TITLE); }, UIIcon::Chess, [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<ChessActivity>(r, i);
      }));

  // Sudoku App
  gamesApps.push_back(std::make_unique<App>(
      []() { return tr(STR_SUDOKU_TITLE); }, UIIcon::Sudoku, [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<SudokuActivity>(r, i);
      }));

  // Dice App
  gamesApps.push_back(std::make_unique<App>(
      []() { return tr(STR_DICE_TITLE); }, UIIcon::Dice, [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<DiceActivity>(r, i);
      }));

  // Tic-Tac-Toe App
  gamesApps.push_back(std::make_unique<App>(
      []() { return tr(STR_TICTACTOE_TITLE); }, UIIcon::TicTacToe,
      [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<TicTacToeActivity>(r, i);
      }));

  // --- Entertainment ---

  // Wikipedia App
  entertainmentApps.push_back(
      std::make_unique<App>([]() { return tr(STR_WIKIPEDIA_TITLE); }, UIIcon::Wikipedia,
                            [](GfxRenderer &r, MappedInputManager &i) {
                              return std::make_unique<WikipediaActivity>(r, i);
                            }));

  // RSS Feed App
  entertainmentApps.push_back(std::make_unique<App>(
      []() { return tr(STR_RSS_TITLE); }, UIIcon::Rss, [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<RssActivity>(r, i);
      }));

  // Formula 1 App
  entertainmentApps.push_back(std::make_unique<App>(
      []() { return tr(STR_F1_TITLE); }, UIIcon::FormulaOne,
      [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<FormulaOneActivity>(r, i);
      }));

  // --- Tools ---

  // Calculator App
  toolsApps.push_back(
      std::make_unique<App>([]() { return tr(STR_CALCULATOR_TITLE); }, UIIcon::Calculator,
                            [](GfxRenderer &r, MappedInputManager &i) {
                              return std::make_unique<CalculatorActivity>(r, i);
                            }));

  // Weather App
  toolsApps.push_back(std::make_unique<App>(
      []() { return tr(STR_WEATHER_TITLE); }, UIIcon::Weather,
      [](GfxRenderer &r, MappedInputManager &i) {
        return std::make_unique<WeatherActivity>(r, i);
      }));

  // DuckDuckGo App
  toolsApps.push_back(
      std::make_unique<App>([]() { return tr(STR_DUCKDUCKGO_TITLE); }, UIIcon::DuckDuckGo,
                            [](GfxRenderer &r, MappedInputManager &i) {
                              return std::make_unique<DuckDuckGoActivity>(r, i);
                            }));
}

const std::vector<std::unique_ptr<App>> &AppRegistry::getCategoryApps(AppCategory category) const {
  switch (category) {
    case AppCategory::Games:
      return gamesApps;
    case AppCategory::Entertainment:
      return entertainmentApps;
    case AppCategory::Tools:
      return toolsApps;
  }
  return gamesApps;
}
