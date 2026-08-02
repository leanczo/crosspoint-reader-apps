#include "RssActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <XmlParserUtils.h>

#include <algorithm>
#include <cctype>

#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/DownloadWatchdog.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

// Selectable font sizes for the article detail view, cycled with Left/Right.
// Default (index 0) is UI_10_FONT_ID -- the same font the feed listing uses
// -- so opening an article doesn't visibly change the text size; Right steps
// up through progressively larger NotoSans sizes for anyone who wants it.
constexpr int kRssArticleFontIds[] = {UI_10_FONT_ID, NOTOSANS_12_FONT_ID, NOTOSANS_14_FONT_ID, NOTOSANS_16_FONT_ID,
                                      NOTOSANS_18_FONT_ID};
constexpr size_t kRssArticleFontCount = sizeof(kRssArticleFontIds) / sizeof(kRssArticleFontIds[0]);
constexpr uint8_t kRssDefaultArticleFontIndex = 0;

size_t findCaseInsensitive(const std::string& str, const std::string& search, size_t pos = 0) {
  if (search.empty() || str.empty() || pos >= str.length()) return std::string::npos;
  auto it = std::search(str.begin() + pos, str.end(), search.begin(), search.end(),
                        [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); });
  if (it == str.end()) return std::string::npos;
  return std::distance(str.begin(), it);
}

// Decodes a numeric (&#NNN;) or hex (&#xNNN;) character reference, or one of
// the common named entities listed below, into its UTF-8 byte sequence.
// `name` is the entity body without the leading '&' or trailing ';' (e.g.
// "amp", "#39", "#x2019"). Returns true and fills `outUtf8` on success.
bool decodeHtmlEntity(const std::string& name, std::string& outUtf8) {
  int code = 0;
  bool decoded = false;
  if (!name.empty() && name[0] == '#') {
    decoded = true;
    if (name.length() > 2 && (name[1] == 'x' || name[1] == 'X')) {
      for (size_t k = 2; k < name.length(); k++) {
        char ch = name[k];
        if (ch >= '0' && ch <= '9') code = code * 16 + (ch - '0');
        else if (ch >= 'a' && ch <= 'f') code = code * 16 + (ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') code = code * 16 + (ch - 'A' + 10);
      }
    } else {
      for (size_t k = 1; k < name.length(); k++) {
        char ch = name[k];
        if (ch >= '0' && ch <= '9') code = code * 10 + (ch - '0');
      }
    }
  } else if (name == "amp") { code = 38; decoded = true; }
  else if (name == "lt") { code = 60; decoded = true; }
  else if (name == "gt") { code = 62; decoded = true; }
  else if (name == "quot") { code = 34; decoded = true; }
  else if (name == "apos") { code = 39; decoded = true; }
  else if (name == "nbsp") { code = 32; decoded = true; }
  else if (name == "ldquo") { code = 8220; decoded = true; }
  else if (name == "rdquo") { code = 8221; decoded = true; }
  else if (name == "lsquo") { code = 8216; decoded = true; }
  else if (name == "rsquo") { code = 8217; decoded = true; }
  else if (name == "ndash") { code = 8211; decoded = true; }
  else if (name == "mdash") { code = 8212; decoded = true; }
  else if (name == "hellip") { code = 8230; decoded = true; }
  else if (name == "euro") { code = 8364; decoded = true; }
  else if (name == "copy") { code = 169; decoded = true; }
  else if (name == "reg") { code = 174; decoded = true; }
  else if (name == "trade") { code = 8482; decoded = true; }

  if (!decoded || code <= 0) return false;

  if (code <= 0x7F) {
    outUtf8 += static_cast<char>(code);
  } else if (code <= 0x7FF) {
    outUtf8 += static_cast<char>(0xC0 | ((code >> 6) & 0x1F));
    outUtf8 += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code <= 0xFFFF) {
    outUtf8 += static_cast<char>(0xE0 | ((code >> 12) & 0x0F));
    outUtf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    outUtf8 += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    outUtf8 += static_cast<char>(0xF0 | ((code >> 18) & 0x07));
    outUtf8 += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    outUtf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    outUtf8 += static_cast<char>(0x80 | (code & 0x3F));
  }
  return true;
}

std::string cleanField(const std::string& input) {
  std::string clean = "";
  clean.reserve(input.length());

  bool lastWasSpace = true;
  bool inTag = false;

  size_t i = 0;
  while (i < input.length()) {
    char c = input[i];

    // Handle HTML entities: named (&amp; &nbsp; &rsquo; &mdash; ...), decimal
    // (&#8217;) and hex (&#x2019;) character references, decoded straight to
    // UTF-8. Entities this doesn't recognize are left as literal text below
    // rather than silently dropped.
    if (c == '&') {
      std::string entity = "";
      size_t j = i;
      while (j < input.length() && j - i < 10) {
        char ec = input[j];
        entity += ec;
        if (ec == ';') break;
        j++;
      }

      std::string replacement;
      size_t entityLen = 0;
      if (entity.length() >= 3 && entity.back() == ';') {
        const std::string name = entity.substr(1, entity.length() - 2);
        if (decodeHtmlEntity(name, replacement)) {
          entityLen = entity.length();
        }
      }

      if (!replacement.empty()) {
        for (char rc : replacement) {
          if (rc == ' ') {
            if (!lastWasSpace) {
              clean += ' ';
              lastWasSpace = true;
            }
          } else {
            clean += rc;
            lastWasSpace = false;
          }
        }
        i += entityLen;
        continue;
      }
      i++;
    } else {
      i++;
    }

    // Handle tag stripping
    if (c == '<') {
      inTag = true;
      continue;
    } else if (c == '>') {
      inTag = false;
      continue;
    }

    if (inTag) {
      continue;
    }

    // Replace whitespace characters and collapse multiple spaces
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      if (!lastWasSpace) {
        clean += ' ';
        lastWasSpace = true;
      }
    } else {
      clean += c;
      lastWasSpace = false;
    }
  }

  if (!clean.empty() && clean.back() == ' ') {
    clean.pop_back();
  }
  return clean;
}

uint32_t parseRssDateToUnix(const std::string& dateStr) {
  if (dateStr.empty()) return 0;

  int year = 1970, month = 1, day = 1;
  int hour = 0, minute = 0, second = 0;

  if (dateStr.length() >= 10 && dateStr[4] == '-' && dateStr[7] == '-') {
    year = std::atoi(dateStr.substr(0, 4).c_str());
    month = std::atoi(dateStr.substr(5, 2).c_str());
    day = std::atoi(dateStr.substr(8, 2).c_str());
    if (dateStr.length() >= 19 && (dateStr[10] == 'T' || dateStr[10] == ' ')) {
      hour = std::atoi(dateStr.substr(11, 2).c_str());
      minute = std::atoi(dateStr.substr(14, 2).c_str());
      second = std::atoi(dateStr.substr(17, 2).c_str());
    }
  } else {
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    size_t mPos = std::string::npos;
    int mIdx = 0;
    for (int i = 0; i < 12; i++) {
      mPos = dateStr.find(months[i]);
      if (mPos != std::string::npos) {
        mIdx = i + 1;
        break;
      }
    }

    if (mPos != std::string::npos) {
      month = mIdx;

      std::string dayStr = "";
      int p = static_cast<int>(mPos) - 1;
      while (p >= 0 && std::isspace(static_cast<unsigned char>(dateStr[p]))) {
        p--;
      }
      while (p >= 0 && std::isdigit(static_cast<unsigned char>(dateStr[p]))) {
        dayStr = dateStr[p] + dayStr;
        p--;
      }
      if (!dayStr.empty()) {
        day = std::atoi(dayStr.c_str());
      }

      std::string yearStr = "";
      size_t yp = mPos + 3;
      while (yp < dateStr.length() && !std::isdigit(static_cast<unsigned char>(dateStr[yp]))) {
        yp++;
      }
      while (yp < dateStr.length() && std::isdigit(static_cast<unsigned char>(dateStr[yp])) && yearStr.length() < 4) {
        yearStr += dateStr[yp];
        yp++;
      }
      if (yearStr.length() == 4) {
        year = std::atoi(yearStr.c_str());
      }

      size_t colonPos = dateStr.find(':');
      if (colonPos != std::string::npos && colonPos >= 2) {
        hour = std::atoi(dateStr.substr(colonPos - 2, 2).c_str());
        minute = std::atoi(dateStr.substr(colonPos + 1, 2).c_str());
        size_t nextColon = dateStr.find(':', colonPos + 1);
        if (nextColon != std::string::npos) {
          second = std::atoi(dateStr.substr(nextColon + 1, 2).c_str());
        }
      }
    }
  }

  if (year < 1970) year = 1970;
  if (month < 1) month = 1;
  if (month > 12) month = 12;
  if (day < 1) day = 1;
  if (day > 31) day = 31;

  static const int daysToMonth[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

  int leapYears = (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;
  long days = (year - 1970) * 365 + leapYears + daysToMonth[month] + (day - 1);

  bool isLeap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  if (isLeap && month > 2) {
    days++;
  }

  return days * 86400 + hour * 3600 + minute * 60 + second;
}

std::string sanitizeFilename(const std::string& input) {
  std::string output = "";
  for (char c : input) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') {
      output += c;
    }
  }
  if (output.length() > 50) {
    output = output.substr(0, 50);
  }
  while (!output.empty() && output.back() == ' ') {
    output.pop_back();
  }
  if (output.empty()) {
    output = "webpage";
  }
  return output;
}

std::string getSanitizedUrlFilename(const std::string& url) {
  std::string clean = "";
  for (char c : url) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      clean += c;
    } else {
      clean += '_';
    }
  }
  std::string result = "";
  bool lastWasUnderscore = false;
  for (char c : clean) {
    if (c == '_') {
      if (!lastWasUnderscore) {
        result += c;
        lastWasUnderscore = true;
      }
    } else {
      result += c;
      lastWasUnderscore = false;
    }
  }
  if (result.length() > 64) {
    result = result.substr(result.length() - 64);
  }
  return result;
}

std::string getFriendlyFeedName(const std::string& url) {
  // Get the text from the URL
  std::string text = url;

  // Replace all non-alphanumeric characters with spaces
  for (char& c : text) {
    if (!std::isalnum(static_cast<unsigned char>(c))) {
      c = ' ';
    }
  }

  // Tokenize and keep only the first three words, skipping boilerplates
  // case-insensitively
  std::vector<std::string> words;
  std::string currentWord = "";
  for (char c : text) {
    if (c == ' ') {
      if (!currentWord.empty()) {
        std::string loweredWord = currentWord;
        for (char& wc : loweredWord) wc = std::tolower(wc);
        if (loweredWord != "http" && loweredWord != "https" && loweredWord != "www" && loweredWord != "com" &&
            loweredWord != "rss" && loweredWord != "xml") {
          words.push_back(currentWord);
        }
        currentWord.clear();
      }
    } else {
      currentWord += c;
    }
  }
  if (!currentWord.empty()) {
    std::string loweredWord = currentWord;
    for (char& wc : loweredWord) wc = std::tolower(wc);
    if (loweredWord != "http" && loweredWord != "https" && loweredWord != "www" && loweredWord != "com" &&
        loweredWord != "rss" && loweredWord != "xml") {
      words.push_back(currentWord);
    }
  }

  std::string friendlyName = "";
  for (size_t i = 0; i < words.size() && i < 3; i++) {
    if (i > 0) friendlyName += " ";
    friendlyName += words[i];
  }

  if (friendlyName.empty()) {
    friendlyName = "Feed";
  }

  return friendlyName;
}

bool loadSingleItemDetails(const std::string& filepath, const std::string& targetLink, std::string& outDesc,
                           std::string& outContent) {
  HalFile file;
  if (!Storage.openFileForRead("RSS", filepath, file)) {
    return false;
  }

  std::string line = "";
  std::string currentLink = "";
  std::string currentDesc = "";
  std::string currentContent = "";
  bool found = false;

  while (file.available() > 0) {
    char c = file.read();
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (line.rfind("## ", 0) == 0) {
        if (found) {
          break;
        }
        currentLink.clear();
        currentDesc.clear();
        currentContent.clear();
      } else {
        if (line.rfind("- Link: ", 0) == 0) {
          currentLink = line.substr(8);
          if (currentLink == targetLink) {
            found = true;
          }
        } else if (found) {
          if (line.rfind("- Description: ", 0) == 0) {
            std::string rawDesc = line.substr(15);
            size_t npos = 0;
            while ((npos = rawDesc.find("\\n", npos)) != std::string::npos) {
              rawDesc.replace(npos, 2, "\n");
              npos += 1;
            }
            currentDesc = rawDesc;
          } else if (line.rfind("- Content: ", 0) == 0) {
            std::string rawContent = line.substr(11);
            size_t npos = 0;
            while ((npos = rawContent.find("\\n", npos)) != std::string::npos) {
              rawContent.replace(npos, 2, "\n");
              npos += 1;
            }
            currentContent = rawContent;
          }
        }
      }
      line = "";
    } else {
      line += c;
    }
  }

  if (!line.empty() && found) {
    if (line.rfind("- Description: ", 0) == 0) {
      std::string rawDesc = line.substr(15);
      size_t npos = 0;
      while ((npos = rawDesc.find("\\n", npos)) != std::string::npos) {
        rawDesc.replace(npos, 2, "\n");
        npos += 1;
      }
      currentDesc = rawDesc;
    } else if (line.rfind("- Content: ", 0) == 0) {
      std::string rawContent = line.substr(11);
      size_t npos = 0;
      while ((npos = rawContent.find("\\n", npos)) != std::string::npos) {
        rawContent.replace(npos, 2, "\n");
        npos += 1;
      }
      currentContent = rawContent;
    }
  }

  file.close();
  if (found) {
    outDesc = currentDesc;
    outContent = currentContent;
    return true;
  }
  return false;
}

class RssParser {
 public:
  RssParser(HalFile& outFile, const std::string& defaultFeedName) : outFile(outFile), defaultFeedName(defaultFeedName) {
    parser = XML_ParserCreate(nullptr);
    if (parser) {
      XML_SetUserData(parser, this);
      XML_SetElementHandler(parser, startElement, endElement);
      XML_SetCharacterDataHandler(parser, characterData);
    }
  }

  ~RssParser() { destroyXmlParser(parser); }

  bool parseBuffer(const char* data, int len, bool isFinal) {
    if (!parser) return false;
    if (XML_Parse(parser, data, len, isFinal) == XML_STATUS_ERROR) {
      LOG_DBG("RSS", "Parse error: %s at line %lu", XML_ErrorString(XML_GetErrorCode(parser)),
              XML_GetCurrentLineNumber(parser));
      return false;
    }
    return true;
  }

  int getItemsParsed() const { return itemsParsed; }

 private:
  HalFile& outFile;
  std::string defaultFeedName;
  XML_Parser parser = nullptr;

  bool inItem = false;
  std::string currentTag = "";
  std::string currentText = "";
  RssItem currentItem;
  int itemsParsed = 0;

  void writeItem(const RssItem& item) {
    outFile.print("## ");
    outFile.print(item.title.c_str());
    outFile.print("\n- Link: ");
    outFile.print(item.link.c_str());
    outFile.print("\n- Source: ");
    outFile.print(item.feedName.c_str());
    outFile.print("\n- Timestamp: ");
    outFile.print(item.timestamp.c_str());
    outFile.print("\n");

    outFile.print("- Description: ");
    std::string escapedDesc = item.description;
    size_t npos = 0;
    while ((npos = escapedDesc.find("\n", npos)) != std::string::npos) {
      escapedDesc.replace(npos, 1, "\\n");
      npos += 2;
    }
    outFile.print(escapedDesc.c_str());
    outFile.print("\n");

    outFile.print("- Content: ");
    std::string escapedContent = item.content;
    npos = 0;
    while ((npos = escapedContent.find("\n", npos)) != std::string::npos) {
      escapedContent.replace(npos, 1, "\\n");
      npos += 2;
    }
    outFile.print(escapedContent.c_str());
    outFile.print("\n\n");
  }

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
    auto* self = static_cast<RssParser*>(userData);
    std::string tag(name);
    std::string lowerTag = tag;
    for (char& c : lowerTag) c = std::tolower(c);

    std::string localTag = lowerTag;
    size_t colonPos = localTag.find(':');
    if (colonPos != std::string::npos) {
      localTag = localTag.substr(colonPos + 1);
    }

    if (localTag == "item" || localTag == "entry") {
      self->inItem = true;
      self->currentItem = RssItem();
      self->currentItem.feedName = self->defaultFeedName;
    }

    if (self->inItem) {
      self->currentTag = localTag;
      self->currentText.clear();

      if (localTag == "link") {
        std::string href = "";
        std::string rel = "";
        for (int i = 0; atts[i]; i += 2) {
          std::string attName(atts[i]);
          for (char& c : attName) c = std::tolower(c);

          std::string localAtt = attName;
          size_t attColon = localAtt.find(':');
          if (attColon != std::string::npos) {
            localAtt = localAtt.substr(attColon + 1);
          }

          if (localAtt == "href") {
            href = atts[i + 1];
          } else if (localAtt == "rel") {
            rel = atts[i + 1];
            for (char& c : rel) c = std::tolower(c);
          }
        }

        if (!href.empty()) {
          // If our current link is empty, or we explicitly got an alternate
          // link, prioritize it. Avoid overwriting a valid link with "self" or
          // "enclosure" feed links.
          if (self->currentItem.link.empty() || rel == "alternate" || (rel != "self" && rel != "enclosure")) {
            self->currentItem.link = href;
          }
        }
      }
    }
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<RssParser*>(userData);
    std::string tag(name);
    std::string lowerTag = tag;
    for (char& c : lowerTag) c = std::tolower(c);

    std::string localTag = lowerTag;
    size_t colonPos = localTag.find(':');
    if (colonPos != std::string::npos) {
      localTag = localTag.substr(colonPos + 1);
    }

    if (localTag == "item" || localTag == "entry") {
      self->inItem = false;
      if (!self->currentItem.title.empty() && self->itemsParsed < 25) {
        self->currentItem.title = cleanField(self->currentItem.title);
        self->currentItem.link = cleanField(self->currentItem.link);
        self->currentItem.description = cleanField(self->currentItem.description);
        self->currentItem.content = cleanField(self->currentItem.content);

        self->writeItem(self->currentItem);
        self->itemsParsed++;
      }
      self->currentItem = RssItem();  // Clear the strings in currentItem to
                                      // reclaim heap immediately
    } else if (self->inItem) {
      if (localTag == "title") {
        if (self->currentItem.title.empty()) self->currentItem.title = self->currentText;
      } else if (localTag == "link") {
        if (self->currentItem.link.empty()) self->currentItem.link = self->currentText;
      } else if (localTag == "description" || localTag == "summary") {
        if (self->currentItem.description.empty()) self->currentItem.description = self->currentText;
      } else if (localTag == "content" || localTag == "encoded" || lowerTag == "content:encoded") {
        if (self->currentItem.content.empty()) self->currentItem.content = self->currentText;
      } else if (localTag == "pubdate" || localTag == "updated" || localTag == "published" || localTag == "date" ||
                 lowerTag == "dc:date") {
        if (self->currentItem.timestamp.empty()) {
          uint32_t ts = parseRssDateToUnix(cleanField(self->currentText));
          self->currentItem.timestamp = std::to_string(ts);
        }
      }
    }

    self->currentTag.clear();
    self->currentText.clear();
  }

  static void XMLCALL characterData(void* userData, const XML_Char* s, const int len) {
    auto* self = static_cast<RssParser*>(userData);
    if (self->inItem && !self->currentTag.empty()) {
      size_t limit = 8192;  // Default limit for content
      if (self->currentTag == "description" || self->currentTag == "summary") {
        limit = 2048;
      } else if (self->currentTag == "title" || self->currentTag == "link") {
        limit = 512;
      }
      if (self->currentText.length() < limit) {
        size_t toAppend = std::min(static_cast<size_t>(len), limit - self->currentText.length());
        self->currentText.append(s, toAppend);
      }
    }
  }
};

bool parseXmlFile(const std::string& xmlPath, const std::string& mdPath, const std::string& defaultFeedName) {
  HalFile inFile;
  if (!Storage.openFileForRead("RSS", xmlPath.c_str(), inFile)) {
    return false;
  }

  HalFile outFile;
  if (!Storage.openFileForWrite("RSS", mdPath.c_str(), outFile)) {
    inFile.close();
    return false;
  }

  // Write feed header
  outFile.print("# ");
  outFile.print(defaultFeedName.c_str());
  outFile.print(" Feed\n\n");

  RssParser parser(outFile, defaultFeedName);
  char buffer[2048];

  while (inFile.available() > 0) {
    int bytesRead = inFile.read(reinterpret_cast<uint8_t*>(buffer), sizeof(buffer));
    if (bytesRead > 0) {
      if (!parser.parseBuffer(buffer, bytesRead, inFile.available() == 0)) {
        break;
      }
    }
  }

  inFile.close();
  outFile.close();
  return parser.getItemsParsed() > 0;
}

std::string timeAgo(uint32_t timestamp) {
  if (timestamp == 0) return "";
  time_t now = time(nullptr);
  if (now < static_cast<time_t>(timestamp)) {
    struct tm tm_info;
    time_t ts = timestamp;
    gmtime_r(&ts, &tm_info);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d", tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour,
             tm_info.tm_min);
    return buf;
  }
  uint32_t diff = now - timestamp;
  if (diff < 60) return "just now";
  if (diff < 3600) return std::to_string(diff / 60) + "m ago";
  if (diff < 86400) return std::to_string(diff / 3600) + "h ago";
  return std::to_string(diff / 86400) + "d ago";
}

static void rssFetchTaskFunc(void* param) {
  RssActivity* activity = static_cast<RssActivity*>(param);
  activity->runBackgroundFetch();
  vTaskDelete(nullptr);
}

}  // namespace

void RssActivity::ensureDirectoriesExist() {
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/rss");
  Storage.ensureDirectoryExists("/apps/rss/content");
}

void RssActivity::loadSubscriptions() {
  subscriptions.clear();
  HalFile f;
  if (!Storage.openFileForRead("RSS", "/apps/rss/subscriptions.txt", f)) {
    subscriptions.push_back("https://news.ycombinator.com/rss");
    subscriptions.push_back("https://www.reddit.com/r/XTEINK/.rss");
    subscriptions.push_back("https://www.reddit.com/.rss");
    subscriptions.push_back("https://news.yahoo.com/rss/mostviewed");
    subscriptions.push_back("https://feeds.bbci.co.uk/news/rss.xml");
    subscriptions.push_back("https://news.google.com/rss?hl=en-US&gl=US&ceid=US:en");
    subscriptions.push_back("https://rss.nytimes.com/services/xml/rss/nyt/HomePage.xml");
    subscriptions.push_back("https://rss.nytimes.com/services/xml/rss/nyt/DiningandWine.xml");
    subscriptions.push_back("https://finance.yahoo.com/news/rssindex");
    subscriptions.push_back("https://www.eltribuno.com/rss-new/salta.rss");
    subscriptions.push_back("https://psicologiaymente.net/feeds/blog/rss");
    subscriptions.push_back("https://rinconpsicologia.com/feed/");

    saveSubscriptions();
    return;
  }

  std::string currentLine = "";
  while (f.available() > 0) {
    char c = f.read();
    if (c == '\n') {
      if (!currentLine.empty() && currentLine.back() == '\r') {
        currentLine.pop_back();
      }
      if (!currentLine.empty()) {
        subscriptions.push_back(currentLine);
      }
      currentLine = "";
    } else {
      currentLine += c;
    }
  }
  if (!currentLine.empty()) {
    subscriptions.push_back(currentLine);
  }
  f.close();
}

void RssActivity::saveSubscriptions() {
  HalFile f;
  if (Storage.openFileForWrite("RSS", "/apps/rss/subscriptions.txt", f)) {
    for (const auto& sub : subscriptions) {
      std::string line = sub + "\n";
      f.write(line.c_str(), line.length());
    }
    f.close();
  }
}

void RssActivity::loadArticleFontSize() {
  articleFontSizeIndex = kRssDefaultArticleFontIndex;
  HalFile f;
  if (Storage.openFileForRead("RSS", "/apps/rss/font_size.txt", f)) {
    char c = f.available() > 0 ? f.read() : '0';
    f.close();
    uint8_t parsed = static_cast<uint8_t>(c - '0');
    if (parsed < kRssArticleFontCount) {
      articleFontSizeIndex = parsed;
    }
  }
}

void RssActivity::saveArticleFontSize() {
  HalFile f;
  if (Storage.openFileForWrite("RSS", "/apps/rss/font_size.txt", f)) {
    char c = static_cast<char>('0' + articleFontSizeIndex);
    f.write(&c, 1);
    f.close();
  }
}

bool RssActivity::parseFeedsFromMarkdown(const std::string& filepath, std::vector<RssItem>& targetList,
                                         bool summaryOnly) {
  HalFile file;
  if (!Storage.openFileForRead("RSS", filepath, file)) {
    return false;
  }

  std::string line = "";
  RssItem currentItem;
  bool inItem = false;

  while (file.available() > 0) {
    char c = file.read();
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (line.rfind("## ", 0) == 0) {
        if (inItem && !currentItem.link.empty()) {
          targetList.push_back(currentItem);
        }
        currentItem = RssItem();
        currentItem.title = line.substr(3);
        inItem = true;
      } else if (inItem) {
        if (line.rfind("- Link: ", 0) == 0) {
          currentItem.link = line.substr(8);
        } else if (line.rfind("- Source: ", 0) == 0) {
          currentItem.feedName = line.substr(10);
        } else if (line.rfind("- Timestamp: ", 0) == 0) {
          currentItem.timestamp = line.substr(13);
        } else if (line.rfind("- Description: ", 0) == 0) {
          std::string rawDesc = line.substr(15);
          size_t npos = 0;
          while ((npos = rawDesc.find("\\n", npos)) != std::string::npos) {
            rawDesc.replace(npos, 2, "\n");
            npos += 1;
          }
          if (summaryOnly && rawDesc.length() > 150) {
            rawDesc = rawDesc.substr(0, 150) + "...";
          }
          currentItem.description = rawDesc;
        } else if (line.rfind("- Content: ", 0) == 0) {
          if (!summaryOnly) {
            std::string rawContent = line.substr(11);
            size_t npos = 0;
            while ((npos = rawContent.find("\\n", npos)) != std::string::npos) {
              rawContent.replace(npos, 2, "\n");
              npos += 1;
            }
            currentItem.content = rawContent;
          }
        }
      }
      line = "";
    } else {
      line += c;
    }
  }

  if (!line.empty()) {
    if (line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind("## ", 0) == 0) {
      if (inItem && !currentItem.link.empty()) {
        targetList.push_back(currentItem);
      }
      currentItem = RssItem();
      currentItem.title = line.substr(3);
      inItem = true;
    } else if (inItem) {
      if (line.rfind("- Link: ", 0) == 0) {
        currentItem.link = line.substr(8);
      } else if (line.rfind("- Source: ", 0) == 0) {
        currentItem.feedName = line.substr(10);
      } else if (line.rfind("- Timestamp: ", 0) == 0) {
        currentItem.timestamp = line.substr(13);
      } else if (line.rfind("- Description: ", 0) == 0) {
        std::string rawDesc = line.substr(15);
        size_t npos = 0;
        while ((npos = rawDesc.find("\\n", npos)) != std::string::npos) {
          rawDesc.replace(npos, 2, "\n");
          npos += 1;
        }
        if (summaryOnly && rawDesc.length() > 150) {
          rawDesc = rawDesc.substr(0, 150) + "...";
        }
        currentItem.description = rawDesc;
      } else if (line.rfind("- Content: ", 0) == 0) {
        if (!summaryOnly) {
          std::string rawContent = line.substr(11);
          size_t npos = 0;
          while ((npos = rawContent.find("\\n", npos)) != std::string::npos) {
            rawContent.replace(npos, 2, "\n");
            npos += 1;
          }
          currentItem.content = rawContent;
        }
      }
    }
  }

  if (inItem && !currentItem.link.empty()) {
    targetList.push_back(currentItem);
  }
  file.close();
  return true;
}

bool RssActivity::loadOfflineFeeds() {
  allItems.clear();
  std::string filename = getSanitizedUrlFilename(activeFeed);
  std::string filepath = "/apps/rss/" + filename + ".md";
  bool success = parseFeedsFromMarkdown(filepath, allItems, true);
  if (success) {
    // Sort globally by timestamp descending
    std::sort(allItems.begin(), allItems.end(), [](const RssItem& a, const RssItem& b) {
      return atoll(a.timestamp.c_str()) > atoll(b.timestamp.c_str());
    });
  }
  return success && !allItems.empty();
}

void RssActivity::writeDebugLog(const std::string& contents) {
  ensureDirectoriesExist();
  HalFile f;
  if (Storage.openFileForWrite("RSS", "/apps/rss/rss_debug.log", f)) {
    f.print(contents.c_str());
  }
}

void RssActivity::runBackgroundFetch() {
  DownloadWatchdog::start(60000);
  errorMessage.clear();

  bool anySuccess = false;
  std::vector<RssItem> aggregatedItems;
  std::string debugLog;

  if (WiFi.status() == WL_CONNECTED) {
    if (!WifiConnectHelper::waitForTimeSync()) {
      errorMessage = "Clock sync failed. HTTPS requests may fail.";
    }
    delay(500);

    if (fetchTaskHandle != nullptr) {
      std::string url = activeFeed;
      std::string filename = getSanitizedUrlFilename(url);
      std::string xmlPath = "/apps/rss/temp.xml";

      Storage.remove(xmlPath.c_str());

      debugLog += "Fetching: " + url + "\n";

      int fetchRetries = 3;
      bool fetchSuccess = false;
      int attempt = 0;

      while (fetchRetries > 0 && !cancelFetch) {
        attempt++;
        std::string errorDetail;
        std::string finalUrl;
        auto res = HttpDownloader::downloadToFile(url, xmlPath, nullptr, &cancelFetch, "", "", nullptr, &finalUrl,
                                                  &errorDetail);
        debugLog += "  Attempt " + std::to_string(attempt) + ": result=" + std::to_string(static_cast<int>(res));
        if (!finalUrl.empty() && finalUrl != url) {
          debugLog += " finalUrl=" + finalUrl;
        }
        if (!errorDetail.empty()) {
          debugLog += " detail=" + errorDetail;
        }
        debugLog += "\n";
        if (res == HttpDownloader::OK) {
          fetchSuccess = true;
          break;
        } else if (res == HttpDownloader::ABORTED) {
          break;
        } else {
          errorMessage = "HTTP Error " + std::to_string(res) + ": " + (errorDetail.empty() ? "Unknown" : errorDetail);
        }
        fetchRetries--;
        if (fetchRetries > 0 && !cancelFetch) {
          delay(1000);
        }
      }
      if (fetchSuccess) {
        std::string friendlyName = getFriendlyFeedName(url);
        std::string mdPath = "/apps/rss/" + filename + ".md";
        ensureDirectoriesExist();
        bool parsed = parseXmlFile(xmlPath, mdPath, friendlyName);
        debugLog += "Parse: " + std::string(parsed ? "OK" : "FAILED") + "\n";
        if (parsed) {
          anySuccess = true;
        }
        Storage.remove(xmlPath.c_str());
      }
    }
  } else {
    debugLog += "WiFi not connected\n";
  }

  writeDebugLog(debugLog);

  DownloadWatchdog::stop();
  if (DownloadWatchdog::gotTimeout) {
    LOG_ERR("RSS", "Background refresh timed out!");
    backgroundFetchSuccess = false;
    errorMessage = "Request timed out.";
  } else {
    backgroundFetchSuccess = anySuccess;
    if (!anySuccess && errorMessage.empty()) {
      errorMessage = "Failed to fetch feeds.";
    }
  }

  if (cancelFetch) {
    fetchTaskHandle = nullptr;
    return;
  }

  pendingUpdateFeed = true;
}

void RssActivity::downloadActivePost() {
  if (selectedItemIndex < 0 || selectedItemIndex >= static_cast<int>(allItems.size())) {
    return;
  }

  std::string downloadUrl = allItems[selectedItemIndex].link;
  std::string downloadTitle = allItems[selectedItemIndex].title;

  // www.reddit.com serves post/comment pages behind a JS bot-challenge to any
  // non-browser HTTP client (no content, just a "solution"/"js_challenge" form).
  // old.reddit.com serves the same content fully server-rendered, so rewrite
  // reddit.com links to it before downloading.
  for (const std::string& prefix : {std::string("https://www.reddit.com/"), std::string("https://reddit.com/")}) {
    if (downloadUrl.rfind(prefix, 0) == 0) {
      downloadUrl = "https://old.reddit.com/" + downloadUrl.substr(prefix.length());
      break;
    }
  }

  // Clear the items list and shrink to fit to free up massive heap RAM
  allItems.clear();
  allItems.shrink_to_fit();

  GUI.drawPopup(renderer, "Downloading...");
  if (WifiConnectHelper::ensureWifiConnected()) {
    wifiWasUsed = true;
    std::string sanitized = sanitizeFilename(downloadTitle);
    if (sanitized.length() > 30) {
      sanitized = sanitized.substr(0, 30);
    }
    Storage.ensureDirectoryExists("/websites");
    std::string tempPath = "/websites/temp_download.tmp";

    bool success = false;
    int retries = 3;
    int attempt = 0;
    std::string debugLog = "Visit Link: " + downloadUrl + "\n";

    while (retries > 0) {
      GUI.drawPopup(renderer, "Downloading...");

      attempt++;
      std::string errorDetail;
      std::string finalUrl;
      auto result = HttpDownloader::downloadToFile(downloadUrl.c_str(), tempPath.c_str(), nullptr, nullptr, "", "",
                                                   nullptr, &finalUrl, &errorDetail);
      debugLog += "  Attempt " + std::to_string(attempt) + ": result=" + std::to_string(static_cast<int>(result));
      if (!finalUrl.empty() && finalUrl != downloadUrl) debugLog += " finalUrl=" + finalUrl;
      if (!errorDetail.empty()) debugLog += " detail=" + errorDetail;
      debugLog += "\n";
      if (result == HttpDownloader::OK) {
        success = true;
        break;
      }
      retries--;
      if (retries > 0) {
        delay(1000);
      }
    }
    writeDebugLog(debugLog);

    if (success) {
      std::string ext = ".html";
      std::string urlToCheck = downloadUrl;

      size_t queryPos = urlToCheck.find('?');
      if (queryPos != std::string::npos) {
        urlToCheck = urlToCheck.substr(0, queryPos);
      }

      bool isTxt = false;
      if (urlToCheck.length() >= 4) {
        std::string urlExt = urlToCheck.substr(urlToCheck.length() - 4);
        for (char& c : urlExt) c = tolower(c);
        if (urlExt == ".txt") {
          isTxt = true;
        }
      }

      if (isTxt) {
        ext = ".txt";
      }

      std::string destPath = "/websites/" + sanitized + ext;
      {
        RenderLock lock;
        if (Storage.exists(destPath.c_str())) {
          Storage.remove(destPath.c_str());
        }
        Storage.rename(tempPath.c_str(), destPath.c_str());
      }

      // Reload feeds and selected post details to restore state
      loadOfflineFeeds();
      if (state == RssState::PostDetail && selectedItemIndex >= 0 &&
          selectedItemIndex < static_cast<int>(allItems.size())) {
        const auto& item = allItems[selectedItemIndex];
        std::string filename = getSanitizedUrlFilename(activeFeed);
        std::string filepath = "/apps/rss/" + filename + ".md";
        loadSingleItemDetails(filepath, item.link, allItems[selectedItemIndex].description,
                              allItems[selectedItemIndex].content);
      }

      activityManager.pushReader(destPath);
    } else {
      // Reload feeds and selected post details to restore state
      loadOfflineFeeds();
      if (state == RssState::PostDetail && selectedItemIndex >= 0 &&
          selectedItemIndex < static_cast<int>(allItems.size())) {
        const auto& item = allItems[selectedItemIndex];
        std::string filename = getSanitizedUrlFilename(activeFeed);
        std::string filepath = "/apps/rss/" + filename + ".md";
        loadSingleItemDetails(filepath, item.link, allItems[selectedItemIndex].description,
                              allItems[selectedItemIndex].content);
      }

      GUI.drawPopup(renderer, "Download failed!");
      delay(2000);
      requestUpdate();
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  } else {
    // Reload feeds and selected post details to restore state
    loadOfflineFeeds();
    if (state == RssState::PostDetail && selectedItemIndex >= 0 &&
        selectedItemIndex < static_cast<int>(allItems.size())) {
      const auto& item = allItems[selectedItemIndex];
      std::string filename = getSanitizedUrlFilename(activeFeed);
      std::string filepath = "/apps/rss/" + filename + ".md";
      loadSingleItemDetails(filepath, item.link, allItems[selectedItemIndex].description,
                            allItems[selectedItemIndex].content);
    }
    requestUpdate();
  }
}

void RssActivity::showQrForActivePost() {
  if (selectedItemIndex < 0 || selectedItemIndex >= static_cast<int>(allItems.size())) return;
  const std::string url = allItems[selectedItemIndex].link;
  if (url.empty()) return;
  startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, url),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void RssActivity::onEnter() {
  Activity::onEnter();
  ensureDirectoriesExist();
  loadSubscriptions();
  loadArticleFontSize();

  state = RssState::FeedSelection;
  activeFeed = "";
  selectedSubIndex = 0;
  isRefreshing = false;
  pendingUpdateFeed = false;
  requestUpdate();
}

void RssActivity::onExit() {
  Activity::onExit();
  DownloadWatchdog::stop();
  if (fetchTaskHandle != nullptr) {
    cancelFetch = true;
    int waitCount = 0;
    while (fetchTaskHandle != nullptr && waitCount < 500) {
      delay(10);
      waitCount++;
    }
    if (fetchTaskHandle != nullptr) {
      LOG_ERR("RSS", "Task failed to exit gracefully, forcing vTaskDelete!");
      TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
      fetchTaskHandle = nullptr;
      vTaskDelete(tempHandle);
    }
  }
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void RssActivity::loop() {
  if (pendingUpdateFeed) {
    pendingUpdateFeed = false;
    isRefreshing = false;
    fetchTaskHandle = nullptr;
    bool timedOut = DownloadWatchdog::gotTimeout;
    loadOfflineFeeds();
    if (timedOut) {
      errorMessage = "Request timed out.";
    } else if (allItems.empty()) {
      errorMessage = "Offline. No cached feed items found.";
    } else {
      errorMessage.clear();
    }
    if (state == RssState::Loading) {
      selectedItemIndex = 0;
      itemsScrollOffset = 0;
      state = RssState::FeedList;
    }
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == RssState::FeedList || state == RssState::Loading) {
      if (fetchTaskHandle != nullptr) {
        cancelFetch = true;
        int waitCount = 0;
        while (fetchTaskHandle != nullptr && waitCount < 500) {
          delay(10);
          waitCount++;
        }
        if (fetchTaskHandle != nullptr) {
          LOG_ERR("RSS", "Task failed to cancel! Forcing kill.");
          TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
          fetchTaskHandle = nullptr;
          vTaskDelete(tempHandle);
        }
      }
      isRefreshing = false;
      pendingUpdateFeed = false;
      activeFeed = "";
      allItems.clear();
      state = RssState::FeedSelection;
      requestUpdate();
    } else if (state == RssState::FeedSelection) {
      finish();
    } else if (state == RssState::PostDetail) {
      loadOfflineFeeds();  // Free RAM by reloading summary-only feed list
      state = RssState::FeedList;
      requestUpdate();
    }
    return;
  }

  if (state == RssState::FeedSelection) {
    int totalItems = static_cast<int>(subscriptions.size()) + 1;  // subscriptions + Add RSS URL
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectedSubIndex = (selectedSubIndex - 1 + totalItems) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectedSubIndex = (selectedSubIndex + 1) % totalItems;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedSubIndex == totalItems - 1) {
        // [+ Add RSS URL]
        auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Add RSS URL", "https://", 150);
        startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
            if (keyboardResult && !keyboardResult->text.empty() && keyboardResult->text != "https://") {
              std::string url = keyboardResult->text;
              if (std::find(subscriptions.begin(), subscriptions.end(), url) == subscriptions.end()) {
                subscriptions.push_back(url);
                saveSubscriptions();
              }
              activeFeed = url;
              bool hasCache = loadOfflineFeeds();
              selectedItemIndex = 0;
              itemsScrollOffset = 0;
              errorMessage = "";
              if (hasCache) {
                state = RssState::FeedList;
                isRefreshing = true;
                requestUpdate();
                ensureWifiConnected(
                    [this]() {
                      wifiWasUsed = true;
                      xTaskCreate(rssFetchTaskFunc, "rss_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
                    },
                    [this]() {
                      isRefreshing = false;
                      requestUpdate();
                    });
              } else {
                state = RssState::Loading;
                isRefreshing = true;
                requestUpdate();
                ensureWifiConnected(
                    [this]() {
                      wifiWasUsed = true;
                      xTaskCreate(rssFetchTaskFunc, "rss_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
                    },
                    [this]() {
                      state = RssState::FeedSelection;
                      isRefreshing = false;
                      requestUpdate();
                    });
              }
              return;
            }
          }
          requestUpdate();
        });
      } else {
        if (selectedSubIndex == totalItems - 1) {
          // Add URL
        } else {
          activeFeed = subscriptions[selectedSubIndex];
        }

        bool hasCache = loadOfflineFeeds();
        selectedItemIndex = 0;
        itemsScrollOffset = 0;
        errorMessage = "";

        if (hasCache) {
          state = RssState::FeedList;
          isRefreshing = false;
          requestUpdate();
        } else {
          state = RssState::Loading;
          isRefreshing = true;
          requestUpdate();
          ensureWifiConnected(
              [this]() {
                wifiWasUsed = true;
                xTaskCreate(rssFetchTaskFunc, "rss_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
              },
              [this]() {
                state = RssState::FeedSelection;
                isRefreshing = false;
                requestUpdate();
              });
        }
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      // Right button to delete RSS subscription
      if (selectedSubIndex >= 0 && selectedSubIndex < totalItems - 1) {
        std::string subToDelete = subscriptions[selectedSubIndex];
        auto handler = [this, subToDelete](const ActivityResult& res) {
          if (!res.isCancelled) {
            auto it = std::find(subscriptions.begin(), subscriptions.end(), subToDelete);
            if (it != subscriptions.end()) {
              subscriptions.erase(it);
              saveSubscriptions();
              selectedSubIndex = 0;
              std::string filename = getSanitizedUrlFilename(subToDelete);
              std::string filepath = "/apps/rss/" + filename + ".md";
              Storage.remove(filepath.c_str());
            }
          }
          requestUpdate();
        };
        startActivityForResult(
            std::make_unique<ConfirmationActivity>(renderer, mappedInput, "Unsubscribe?", subToDelete), handler);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      // Left button to edit the URL of an existing RSS subscription
      if (selectedSubIndex >= 0 && selectedSubIndex < totalItems - 1) {
        std::string oldUrl = subscriptions[selectedSubIndex];
        auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Edit RSS URL", oldUrl, 150);
        startActivityForResult(std::move(keyboard), [this, oldUrl](const ActivityResult& result) {
          if (!result.isCancelled) {
            auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
            if (keyboardResult && !keyboardResult->text.empty() && keyboardResult->text != oldUrl &&
                std::find(subscriptions.begin(), subscriptions.end(), keyboardResult->text) == subscriptions.end()) {
              auto it = std::find(subscriptions.begin(), subscriptions.end(), oldUrl);
              if (it != subscriptions.end()) {
                *it = keyboardResult->text;
                saveSubscriptions();
                // Old cache is keyed off the old URL's filename hash; drop it so the
                // edited URL fetches fresh content under its own cache file.
                std::string oldFilename = getSanitizedUrlFilename(oldUrl);
                Storage.remove(("/apps/rss/" + oldFilename + ".md").c_str());
                if (activeFeed == oldUrl) {
                  activeFeed = keyboardResult->text;
                }
              }
            }
          }
          requestUpdate();
        });
      }
    }
  } else if (state == RssState::FeedList) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      isRefreshing = true;
      state = RssState::Loading;
      requestUpdate();
      ensureWifiConnected(
          [this]() {
            wifiWasUsed = true;
            xTaskCreate(rssFetchTaskFunc, "rss_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
          },
          [this]() {
            state = RssState::FeedList;
            isRefreshing = false;
            requestUpdate();
          });
      return;
    }
    if (allItems.empty()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        isRefreshing = true;
        state = RssState::Loading;
        requestUpdate();
        ensureWifiConnected(
            [this]() {
              wifiWasUsed = true;
              xTaskCreate(rssFetchTaskFunc, "rss_fetch", 8192, this, 5, (TaskHandle_t*)&fetchTaskHandle);
            },
            [this]() {
              state = RssState::FeedList;
              isRefreshing = false;
              requestUpdate();
            });
        return;
      }
    } else {
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        if (selectedItemIndex > 0) {
          selectedItemIndex--;
          if (selectedItemIndex < itemsScrollOffset) {
            itemsScrollOffset = selectedItemIndex;
          }
          requestUpdate();
        }
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        if (selectedItemIndex < static_cast<int>(allItems.size()) - 1) {
          selectedItemIndex++;
          if (selectedItemIndex >= itemsScrollOffset + 5) {
            itemsScrollOffset = selectedItemIndex - 4;
          }
          requestUpdate();
        }
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        // Load details dynamically from the markdown file on disk to save RAM
        const auto& item = allItems[selectedItemIndex];
        std::string filename = getSanitizedUrlFilename(activeFeed);
        std::string filepath = "/apps/rss/" + filename + ".md";
        loadSingleItemDetails(filepath, item.link, allItems[selectedItemIndex].description,
                              allItems[selectedItemIndex].content);

        state = RssState::PostDetail;
        detailScrollOffset = 0;
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        downloadActivePost();
      }
    }
  } else if (state == RssState::PostDetail) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (detailScrollOffset > 0) {
        detailScrollOffset = std::max(0, detailScrollOffset - detailMaxLines);
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      // render() re-clamps this to the last valid page, so it's safe to
      // overshoot here — a full-page jump instead of 1 line so the screen
      // fully replaces instead of shifting by a single row each press.
      detailScrollOffset += detailMaxLines;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      postActionMenuIndex = 0;
      state = RssState::PostActionMenu;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (articleFontSizeIndex > 0) {
        articleFontSizeIndex--;
        saveArticleFontSize();
        requestUpdate();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (articleFontSizeIndex + 1 < kRssArticleFontCount) {
        articleFontSizeIndex++;
        saveArticleFontSize();
        requestUpdate();
      }
    }
  } else if (state == RssState::PostActionMenu) {
    constexpr int kActionCount = 2;  // Visit Link, Show QR
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = RssState::PostDetail;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      postActionMenuIndex = (postActionMenuIndex - 1 + kActionCount) % kActionCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      postActionMenuIndex = (postActionMenuIndex + 1) % kActionCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (postActionMenuIndex == 0) {
        // downloadActivePost() restores article state assuming it's called
        // from PostDetail (see its `state == RssState::PostDetail` checks on
        // failure/offline paths), so switch back before invoking it.
        state = RssState::PostDetail;
        downloadActivePost();
      } else {
        showQrForActivePost();
      }
    }
  }
}

void RssActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string headerTitle = "RSS Feed";
  if (state == RssState::FeedList || state == RssState::Loading) {
    headerTitle = getFriendlyFeedName(activeFeed);
  }

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle.c_str());

  if (state == RssState::FeedList && isRefreshing) {
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, metrics.topPadding + 5, "Refreshing feed...", true,
                      EpdFontFamily::REGULAR);
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  if (state == RssState::Loading) {
    int textY = contentTop + contentHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, "Loading feeds...");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == RssState::FeedList) {
    if (!errorMessage.empty() && allItems.empty()) {
      int errY = contentTop + 40;
      renderer.drawCenteredText(UI_10_FONT_ID, errY, errorMessage.c_str(), true, EpdFontFamily::BOLD);
    } else {
      const int listTop = contentTop;
      const int cellH = 115;
      const int spacing = 10;
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int sectionGap = 6;

      for (int i = 0; i < 5; i++) {
        int idx = itemsScrollOffset + i;
        if (idx >= static_cast<int>(allItems.size())) break;

        const auto& item = allItems[idx];
        int cellY = listTop + i * (cellH + spacing);
        int cellX = metrics.contentSidePadding;
        int cellW = pageWidth - 2 * metrics.contentSidePadding;
        int cellBottom = cellY + cellH - 4;

        bool isSelected = (idx == selectedItemIndex);
        renderer.drawRoundedRect(cellX, cellY, cellW, cellH, isSelected ? 3 : 1, 8, true);

        std::string metadata = item.feedName;
        long long ts = atoll(item.timestamp.c_str());
        std::string relativeTime = timeAgo(ts);
        if (!relativeTime.empty()) {
          metadata += " • " + relativeTime;
        }
        int textY = cellY + 12;
        renderer.drawText(UI_10_FONT_ID, cellX + 12, textY, metadata.c_str(), true, EpdFontFamily::BOLD);
        textY += lineHeight + sectionGap;

        auto titleLines =
            renderer.wrappedText(UI_10_FONT_ID, item.title.c_str(), cellW - 24, 2, EpdFontFamily::REGULAR);
        for (size_t l = 0; l < titleLines.size() && l < 2 && textY + lineHeight <= cellBottom; l++) {
          renderer.drawText(UI_10_FONT_ID, cellX + 12, textY, titleLines[l].c_str(), true,
                            EpdFontFamily::REGULAR);
          textY += lineHeight;
        }
        textY += sectionGap;

        if (!item.description.empty()) {
          auto descLines =
              renderer.wrappedText(UI_10_FONT_ID, item.description.c_str(), cellW - 24, 2, EpdFontFamily::REGULAR);
          for (size_t l = 0; l < descLines.size() && l < 2 && textY + lineHeight <= cellBottom; l++) {
            renderer.drawText(UI_10_FONT_ID, cellX + 12, textY, descLines[l].c_str(), true,
                              EpdFontFamily::REGULAR);
            textY += lineHeight;
          }
        }
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "Details", "Refresh");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == RssState::PostDetail) {
    const auto& item = allItems[selectedItemIndex];
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "RSS Post");

    int contentY = contentTop;
    const int articleFontId = kRssArticleFontIds[articleFontSizeIndex];

    auto titleLines = renderer.wrappedText(articleFontId, item.title.c_str(),
                                           pageWidth - 2 * metrics.contentSidePadding, 3, EpdFontFamily::BOLD);
    for (size_t l = 0; l < titleLines.size(); l++) {
      renderer.drawText(articleFontId, metrics.contentSidePadding, contentY + l * renderer.getLineHeight(articleFontId),
                        titleLines[l].c_str(), true, EpdFontFamily::BOLD);
    }
    int titleHeight = titleLines.size() * renderer.getLineHeight(articleFontId);
    contentY += titleHeight + 10;

    std::string fullText = item.description;
    if (!item.content.empty()) {
      if (!fullText.empty()) fullText += "\n\n";
      fullText += item.content;
    }

    if (!item.link.empty()) {
      std::string printableUrl = item.link;
      std::string formattedUrl = "";
      for (size_t i = 0; i < printableUrl.length(); ++i) {
        formattedUrl += printableUrl[i];
        if (printableUrl[i] == '?' || printableUrl[i] == '&' || printableUrl[i] == '-' || printableUrl[i] == '_') {
          formattedUrl += " ";
        } else if (printableUrl[i] == '/') {
          if (i + 1 < printableUrl.length() && printableUrl[i + 1] != '/') {
            formattedUrl += " ";
          }
        } else if (printableUrl[i] == '.') {
          if (i > 0 && !std::isdigit(printableUrl[i - 1])) {
            formattedUrl += " ";
          }
        }
      }
      if (!fullText.empty()) fullText += "\n\n";
      fullText += "Link: " + formattedUrl;
    }

    auto lines = renderer.wrappedText(articleFontId, fullText.c_str(), pageWidth - 2 * metrics.contentSidePadding, 500,
                                      EpdFontFamily::REGULAR);

    int maxLines = (contentHeight - (contentY - contentTop)) / renderer.getLineHeight(articleFontId);
    detailMaxLines = std::max(1, maxLines);
    if (detailScrollOffset > std::max(0, static_cast<int>(lines.size()) - maxLines)) {
      detailScrollOffset = std::max(0, static_cast<int>(lines.size()) - maxLines);
    }

    for (int i = 0; i < maxLines; i++) {
      int lineIdx = detailScrollOffset + i;
      if (lineIdx >= static_cast<int>(lines.size())) break;
      renderer.drawText(articleFontId, metrics.contentSidePadding, contentY + i * renderer.getLineHeight(articleFontId),
                        lines[lineIdx].c_str(), true, EpdFontFamily::REGULAR);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == RssState::PostActionMenu) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "RSS Post");

    const int menuTop = contentTop;
    const int menuBottom = contentBottom;
    constexpr int kActionCount = 2;
    GUI.drawButtonMenu(
        renderer, Rect{0, menuTop, pageWidth, menuBottom - menuTop}, kActionCount, postActionMenuIndex,
        [](int index) -> std::string { return index == 0 ? tr(STR_RSS_VISIT_LINK) : tr(STR_RSS_SHOW_QR); },
        [](int) { return UIIcon::Library; }, 6);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == RssState::FeedSelection) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "RSS Feed");

    int totalItems = static_cast<int>(subscriptions.size()) + 1;

    GUI.drawButtonMenu(
        renderer,
        Rect{0, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, pageWidth,
             pageHeight -
                 (metrics.headerHeight + metrics.topPadding + metrics.verticalSpacing + metrics.buttonHintsHeight)},
        totalItems, selectedSubIndex,
        [this, totalItems](int index) {
          if (index == totalItems - 1) return std::string("[+ Add RSS URL]");
          std::string url = subscriptions[index];
          return getFriendlyFeedName(url);
        },
        [this, totalItems](int index) {
          if (index == totalItems - 1) return UIIcon::File;
          return UIIcon::Library;
        },
        9);

    const char* leftAction = nullptr;
    const char* rightAction = nullptr;
    if (selectedSubIndex >= 0 && selectedSubIndex < totalItems - 1) {
      leftAction = "Edit";
      rightAction = "Delete";
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), leftAction, rightAction);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
