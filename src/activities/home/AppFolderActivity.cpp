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

  // maxPageItems defaults to 6 (tuned for HomeActivity, where the menu shares
  // the screen with cover art). This folder screen has the full content area
  // to itself, so raise the cap and let drawButtonMenu's own
  // rect.height/rowHeight math decide how many rows actually fit — otherwise
  // it paginates at 6 even when there's room to show every app without
  // scrolling. Capped at 12, not something larger: LyraTheme::drawButtonMenu
  // switches to a denser 36px/UI_10 row style whenever maxPageItems > 12
  // (meant for long lists like Settings), which would shrink every row here
  // too even though no category has anywhere near that many apps yet. 12
  // keeps normal-size rows and still lets a future category scroll instead
  // of shrinking if it ever grows past that.
  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), selectorIndex,
      [&menuItems](int index) { return menuItems[index]; },
      [&menuIcons](int index) { return menuIcons[index]; }, 12);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, ButtonArrow::Up,
                      ButtonArrow::Down);

  renderer.displayBuffer();
}
