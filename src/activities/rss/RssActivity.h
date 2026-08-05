#pragma once

#include "activities/Activity.h"
#include <string>
#include <vector>
#include <functional>

struct RssItem {
  std::string title;
  std::string link;
  std::string description;
  std::string content;   // Full article content (if available)
  std::string timestamp; // Unix timestamp as string
  std::string feedName;  // display name of the feed
};

struct RssSubscription {
  std::string url;
  std::string customName;  // empty = no override, fall back to getFriendlyFeedName(url)
};

enum class RssState {
  FeedSelection,
  Loading,
  FeedList,
  PostDetail,
  // Reached by pressing Confirm from PostDetail — a small 2-item menu
  // (Visit Link / Show QR) since PostDetail's 4 buttons are all already
  // spoken for (scroll, font size, back).
  PostActionMenu,
  // Reached by pressing Right on a subscription row in FeedSelection — a
  // small 3-item menu (Edit Title / Edit URL / Delete Feed), replacing the
  // old direct Left=EditURL/Right=Delete shortcuts so a single stray press
  // can no longer trigger a delete.
  FeedActionMenu
};

class RssActivity final : public Activity {
 private:
   RssState state = RssState::FeedSelection;
   std::vector<RssItem> allItems;
   std::vector<RssSubscription> subscriptions;
   std::string activeFeed;

   int selectedItemIndex = 0;
   int selectedSubIndex = 0;
   int itemsScrollOffset = 0;
   int detailScrollOffset = 0;
   int postActionMenuIndex = 0;  // which item is highlighted in PostActionMenu
   int feedActionMenuIndex = 0;  // which item is highlighted in FeedActionMenu
   // Lines-per-screen for the article body at the current font size, computed
   // by render() and reused by loop() so Up/Down scroll by a full page
   // instead of a single line (avoids the old "scrolls one row, most of the
   // screen repeats" behavior).
   int detailMaxLines = 1;
   uint8_t articleFontSizeIndex = 0; // index into kRssArticleFontIds, persisted across sessions

   bool offlineMode = false;
   std::string errorMessage;
   bool isRefreshing = false;
   bool pendingUpdateFeed = false;
   bool backgroundFetchSuccess = false;
   void* fetchTaskHandle = nullptr;
   bool cancelFetch = false;
   bool wifiWasUsed = false;

   void loadSubscriptions();
   void saveSubscriptions();
   // customName if set for `url`, else the auto-derived getFriendlyFeedName(url).
   std::string displayNameForUrl(const std::string &url) const;
   bool loadOfflineFeeds();
   void ensureDirectoriesExist();
   void loadArticleFontSize();
   void saveArticleFontSize();

   bool parseFeedsFromMarkdown(const std::string &filepath, std::vector<RssItem> &targetList, bool summaryOnly = false);
   void downloadActivePost();
   void showQrForActivePost();
   // Overwrites /apps/rss/rss_debug.log with the given contents, so the last fetch
   // attempt's HTTP result/error detail can be inspected from the SD card without
   // a serial connection.
   void writeDebugLog(const std::string &contents);

 public:
  void runBackgroundFetch();
  explicit RssActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RssFeed", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
