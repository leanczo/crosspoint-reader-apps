#pragma once

#include "activities/Activity.h"
#include "activities/AppRegistry.h"
#include "util/ButtonNavigator.h"

// Generic "folder" screen that lists a subset of AppRegistry apps grouped by
// category (Games/Entertainment/Tools). Mirrors HomeActivity's app-menu
// rendering/navigation, without the recent-books/cover section.
class AppFolderActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  const AppCategory category;

  std::string getTitle() const;

 public:
  AppFolderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, AppCategory category)
      : Activity("AppFolder", renderer, mappedInput), category(category) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
