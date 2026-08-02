#include "AppFolderActivity.h"

#include <I18n.h>

#include "components/UITheme.h"

std::string AppFolderActivity::getTitle() const {
  switch (category) {
    case AppCategory::Games:
      return tr(STR_FOLDER_GAMES);
    case AppCategory::Entertainment:
      return tr(STR_FOLDER_ENTERTAINMENT);
    case AppCategory::Tools:
      return tr(STR_FOLDER_TOOLS);
  }
  return "";
}

void AppFolderActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void AppFolderActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const auto& apps = AppRegistry::getInstance().getCategoryApps(category);
  const int menuCount = static_cast<int>(apps.size());

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex >= 0 && selectorIndex < menuCount) {
      auto appActivity = apps[selectorIndex]->createActivity(renderer, mappedInput);
      activityManager.pushActivity(std::move(appActivity));
    }
  }
}

void AppFolderActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, getTitle().c_str());

  const auto& apps = AppRegistry::getInstance().getCategoryApps(category);
  std::vector<std::string> menuItems;
  std::vector<UIIcon> menuIcons;
  menuItems.reserve(apps.size());
  menuIcons.reserve(apps.size());
  for (const auto& app : apps) {
    menuItems.push_back(app->getName());
    menuIcons.push_back(app->getIcon());
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), selectorIndex,
      [&menuItems](int index) { return menuItems[index]; },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
