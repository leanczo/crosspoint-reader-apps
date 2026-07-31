#pragma once

#include <vector>
#include <memory>
#include "App.h"

enum class AppCategory { Games, Entertainment, Tools };

class AppRegistry {
 public:
  static AppRegistry& getInstance();
  const std::vector<std::unique_ptr<App>>& getApps() const { return apps; }
  const std::vector<std::unique_ptr<App>>& getCategoryApps(AppCategory category) const;

 private:
  AppRegistry();
  std::vector<std::unique_ptr<App>> apps;
  std::vector<std::unique_ptr<App>> gamesApps;
  std::vector<std::unique_ptr<App>> entertainmentApps;
  std::vector<std::unique_ptr<App>> toolsApps;
};
