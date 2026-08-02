#include "ClockActivity.h"
#include <HalClock.h>
#include "CrossPointSettings.h"
#include "I18n.h"
#include "fontIds.h"
#include "components/UITheme.h"
#include "activities/settings/ClockSyncActivity.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr unsigned long LONG_PRESS_MS = 1000;
}  // namespace

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 5x7 block font for digits 0-9
static const uint8_t font5x7[10][7] = {
  {0b11111,
   0b10001,
   0b10001,
   0b10001,
   0b10001,
   0b10001,
   0b11111}, // 0
  {0b00100,
   0b01100,
   0b00100,
   0b00100,
   0b00100,
   0b00100,
   0b01110}, // 1
  {0b11111,
   0b00001,
   0b00001,
   0b11111,
   0b10000,
   0b10000,
   0b11111}, // 2
  {0b11111,
   0b00001,
   0b00001,
   0b11111,
   0b00001,
   0b00001,
   0b11111}, // 3
  {0b10001,
   0b10001,
   0b10001,
   0b11111,
   0b00001,
   0b00001,
   0b00001}, // 4
  {0b11111,
   0b10000,
   0b10000,
   0b11111,
   0b00001,
   0b00001,
   0b11111}, // 5
  {0b11111,
   0b10000,
   0b10000,
   0b11111,
   0b10001,
   0b10001,
   0b11111}, // 6
  {0b11111,
   0b00001,
   0b00001,
   0b00010,
   0b00100,
   0b01000,
   0b01000}, // 7
  {0b11111,
   0b10001,
   0b10001,
   0b11111,
   0b10001,
   0b10001,
   0b11111}, // 8
  {0b11111,
   0b10001,
   0b10001,
   0b11111,
   0b00001,
   0b00001,
   0b11111}  // 9
};

// Correct Midpoint Circle helper (Bresenham's)
static void drawCircle(const GfxRenderer& renderer, int x0, int y0, int radius, int thickness, bool state) {
  for (int t = 0; t < thickness; t++) {
    int r = radius - t;
    int x = r;
    int y = 0;
    int err = 3 - 2 * r;
    while (x >= y) {
      renderer.drawPixel(x0 + x, y0 + y, state);
      renderer.drawPixel(x0 + y, y0 + x, state);
      renderer.drawPixel(x0 - y, y0 + x, state);
      renderer.drawPixel(x0 - x, y0 + y, state);
      renderer.drawPixel(x0 - x, y0 - y, state);
      renderer.drawPixel(x0 - y, y0 - x, state);
      renderer.drawPixel(x0 + y, y0 - x, state);
      renderer.drawPixel(x0 + x, y0 - y, state);
      
      if (err >= 0) {
        x -= 1;
        err += 4 * (y - x) + 10;
      } else {
        err += 4 * y + 6;
      }
      y += 1;
    }
  }
}

static void fillCircle(const GfxRenderer& renderer, int x0, int y0, int radius, bool state) {
  int x = radius;
  int y = 0;
  int err = 3 - 2 * radius;
  while (x >= y) {
    renderer.drawLine(x0 - x, y0 + y, x0 + x, y0 + y, state);
    renderer.drawLine(x0 - y, y0 + x, x0 + y, y0 + x, state);
    renderer.drawLine(x0 - x, y0 - y, x0 + x, y0 - y, state);
    renderer.drawLine(x0 - y, y0 - x, x0 + y, y0 - x, state);
    if (err >= 0) {
      x -= 1;
      err += 4 * (y - x) + 10;
    } else {
      err += 4 * y + 6;
    }
    y += 1;
  }
}


ClockActivity::ClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Clock", renderer, mappedInput),
      mode(ClockMode::Digital),
      use12Hour(SETTINGS.clockFormat == 1),
      lastHour(99),
      lastMinute(99) {}

void ClockActivity::onEnter() {
  Activity::onEnter();
  use12Hour = (SETTINGS.clockFormat == 1);
  lastHour = 99;
  lastMinute = 99;
  lastTimeCheck = 0;
  requestUpdate();
}

void ClockActivity::getLocalTime(uint8_t& localHour, uint8_t& localMin) {
  uint8_t h = 0, m = 0;
  if (!halClock.getTime(h, m)) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    h = timeinfo.tm_hour;
    m = timeinfo.tm_min;
  }
  int offsetQuarterHours = static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;
  localHour = totalMinutes / 60;
  localMin = totalMinutes % 60;
}

const char* ClockActivity::modeShortName(ClockMode m) {
  switch (m) {
    case ClockMode::Analog:
      return tr(STR_CLOCK_MODE_ANALOG);
    case ClockMode::Digital:
      return tr(STR_CLOCK_MODE_DIGITAL);
    case ClockMode::Flip:
      return tr(STR_CLOCK_MODE_FLIP);
    case ClockMode::Stopwatch:
      return tr(STR_CLOCK_STOPWATCH_TITLE);
    case ClockMode::Timer:
      return tr(STR_CLOCK_TIMER_TITLE);
  }
  return "";
}

// Long-press Back: connect to WiFi if needed, then force an NTP resync via
// the same ClockSyncActivity screen Settings uses (bypasses the
// once-per-device clockHasBeenSynced debounce that ensureWifiConnected's own
// opportunistic sync respects, so this always actually re-syncs on request).
void ClockActivity::syncClock() {
  ensureWifiConnected(
      [this]() {
        startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput),
                                [this](const ActivityResult&) {
                                  lastHour = 99;
                                  lastMinute = 99;
                                  lastTimeCheck = 0;
                                  requestUpdate();
                                });
      },
      [this]() { requestUpdate(); });
}

bool ClockActivity::preventAutoSleep() {
  // Keep the device awake while a countdown/stopwatch is actively running —
  // both live entirely in this Activity's RAM (see the class comment on
  // timerEndMs), so falling asleep and getting evicted would silently lose
  // the running timer.
  return (mode == ClockMode::Stopwatch && stopwatchRunning) || (mode == ClockMode::Timer && timerRunning);
}

void ClockActivity::loop() {
  unsigned long now = millis();

  if (mode == ClockMode::Stopwatch) {
    // Display value is always recomputed live from millis() in
    // drawStopwatch(); this tick only exists to trigger the once-a-second
    // redraw while running (e-ink has no business refreshing faster).
    if (stopwatchRunning && now - lastTimeCheck >= 1000) {
      lastTimeCheck = now;
      requestUpdate();
    }
  } else if (mode == ClockMode::Timer) {
    if (timerRunning && now - lastTimeCheck >= 1000) {
      lastTimeCheck = now;
      long msLeft = static_cast<long>(timerEndMs - now);
      if (msLeft <= 0) {
        timerRemainingSec = 0;
        timerRunning = false;
        timerFinished = true;
      } else {
        timerRemainingSec = static_cast<int>((msLeft + 999) / 1000);
      }
      requestUpdate();
    }
  } else {
    if (now - lastTimeCheck >= 1000 || lastHour == 99) {
      lastTimeCheck = now;
      uint8_t h, m;
      getLocalTime(h, m);
      if (h != lastHour || m != lastMinute) {
        lastHour = h;
        lastMinute = m;
        requestUpdate();
      }
    }
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    syncClock();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    mode = static_cast<ClockMode>((static_cast<int>(mode) + MODE_COUNT - 1) % MODE_COUNT);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    mode = static_cast<ClockMode>((static_cast<int>(mode) + 1) % MODE_COUNT);
    requestUpdate();
    return;
  }

  if (mode == ClockMode::Stopwatch) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (stopwatchRunning) {
        stopwatchElapsedMs += now - stopwatchStartMs;
        stopwatchRunning = false;
      } else {
        stopwatchStartMs = now;
        stopwatchRunning = true;
      }
      requestUpdate();
    } else if (!stopwatchRunning && mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      // Reset only allowed while stopped, so a mistimed press mid-run can't
      // wipe out an in-progress measurement.
      stopwatchElapsedMs = 0;
      requestUpdate();
    }
    return;
  }

  if (mode == ClockMode::Timer) {
    if (timerFinished) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        timerFinished = false;
        timerRemainingSec = timerDurationSec;
        requestUpdate();
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (timerRunning) {
        long msLeft = static_cast<long>(timerEndMs - now);
        timerRemainingSec = static_cast<int>(std::max<long>(0, (msLeft + 999) / 1000));
        timerRunning = false;
      } else if (timerRemainingSec > 0) {
        timerEndMs = now + static_cast<unsigned long>(timerRemainingSec) * 1000UL;
        timerRunning = true;
      }
      requestUpdate();
    } else if (!timerRunning) {
      // Duration is only adjustable while idle — 1-minute steps, held-repeat
      // via ButtonNavigator so dialing in e.g. 25 minutes doesn't take 25
      // separate presses.
      buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
        timerDurationSec = std::min(99 * 60, timerDurationSec + 60);
        timerRemainingSec = timerDurationSec;
        requestUpdate();
      });
      buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
        timerDurationSec = std::max(60, timerDurationSec - 60);
        timerRemainingSec = timerDurationSec;
        requestUpdate();
      });
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    use12Hour = !use12Hour;
    requestUpdate();
  }
}


void ClockActivity::drawDigit(int x, int y, int digit, int blockSize, Color color) {
  if (digit < 0 || digit > 9) return;
  bool state = (color == Color::Black);
  for (int row = 0; row < 7; row++) {
    for (int col = 0; col < 5; col++) {
      if ((font5x7[digit][row] >> (4 - col)) & 1) {
        renderer.fillRect(x + col * blockSize, y + row * blockSize, blockSize, blockSize, state);
      }
    }
  }
}

void ClockActivity::drawColon(int x, int y, int blockSize, Color color) {
  bool state = (color == Color::Black);
  // Draws two square dots representing a colon
  renderer.fillRect(x, y + 2 * blockSize, blockSize, blockSize, state);
  renderer.fillRect(x, y + 4 * blockSize, blockSize, blockSize, state);
}

void ClockActivity::drawAnalogClock(const ThemeMetrics& metrics, int contentTop, int contentHeight, int pageWidth, uint8_t hour, uint8_t minute) {
  int cx = pageWidth / 2;
  int cy = contentTop + contentHeight / 2;
  int radius = 120;

  // Draw Clock Circle (use thickness 1 for faster and cleaner rendering)
  drawCircle(renderer, cx, cy, radius, 1, true);

  // Draw 12 hour ticks
  for (int i = 0; i < 12; i++) {
    float angleRad = i * (float)M_PI / 6.0f;
    int tickLength = (i % 3 == 0) ? 14 : 8;
    int x1 = cx + (int)((radius - tickLength) * sinf(angleRad));
    int y1 = cy - (int)((radius - tickLength) * cosf(angleRad));
    int x2 = cx + (int)(radius * sinf(angleRad));
    int y2 = cy - (int)(radius * cosf(angleRad));
    renderer.drawLine(x1, y1, x2, y2, (i % 3 == 0) ? 2 : 1, true);
  }

  // Calculate hand angles
  float hourAngleRad = ((hour % 12) * 30.0f + minute * 0.5f) * (float)M_PI / 180.0f;
  float minuteAngleRad = (minute * 6.0f) * (float)M_PI / 180.0f;

  int hLength = radius * 0.5;
  int mLength = radius * 0.8;

  int hx = cx + (int)(hLength * sinf(hourAngleRad));
  int hy = cy - (int)(hLength * cosf(hourAngleRad));
  int mx = cx + (int)(mLength * sinf(minuteAngleRad));
  int my = cy - (int)(mLength * cosf(minuteAngleRad));

  // Draw Hands
  renderer.drawLine(cx, cy, hx, hy, 4, true); // Hour hand (Thick)
  renderer.drawLine(cx, cy, mx, my, 2, true); // Minute hand (Medium)

  // Pin Center
  fillCircle(renderer, cx, cy, 6, true);
}

void ClockActivity::drawDigitalClock(const ThemeMetrics& metrics, int contentTop, int contentHeight, int pageWidth, uint8_t hour, uint8_t minute) {
  int blockSize = 18;
  int digitW = 5 * blockSize;
  int digitH = 7 * blockSize;
  int spacing = 18;
  int colonW = 18;

  // Total digital clock width = 4 digits + 2 spacings + colon + 2 spacings
  int totalW = 4 * digitW + 2 * spacing + colonW + 2 * spacing;
  int startX = (pageWidth - totalW) / 2;
  int startY = contentTop + (contentHeight - digitH) / 2;

  uint8_t displayHour = hour;
  if (use12Hour) {
    displayHour = hour % 12;
    if (displayHour == 0) displayHour = 12;
  }

  int h1 = displayHour / 10;
  int h2 = displayHour % 10;
  int m1 = minute / 10;
  int m2 = minute % 10;

  // H1
  drawDigit(startX, startY, h1, blockSize, Color::Black);
  // H2
  drawDigit(startX + digitW + spacing, startY, h2, blockSize, Color::Black);
  // Colon
  drawColon(startX + 2 * digitW + 2 * spacing, startY, blockSize, Color::Black);
  // M1
  drawDigit(startX + 2 * digitW + 2 * spacing + colonW + spacing, startY, m1, blockSize, Color::Black);
  // M2
  drawDigit(startX + 3 * digitW + 3 * spacing + colonW + spacing, startY, m2, blockSize, Color::Black);

  // UTC / Period Suffix Info
  char infoBuf[32];
  double tzOffset = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 0.25;
  snprintf(infoBuf, sizeof(infoBuf), "%s  (UTC%+0.2f)", (use12Hour ? (hour >= 12 ? "PM" : "AM") : "24H"), tzOffset);
  renderer.drawCenteredText(NOTOSANS_14_FONT_ID, startY + digitH + 30, infoBuf);
}

void ClockActivity::drawFlipClock(const ThemeMetrics& metrics, int contentTop, int contentHeight, int pageWidth, uint8_t hour, uint8_t minute) {
  int cardW = 90;
  int cardH = 136;
  int spacing = 10;
  int centerGap = 25;

  int totalW = 4 * cardW + 2 * spacing + centerGap;
  int startX = (pageWidth - totalW) / 2;
  int startY = contentTop + (contentHeight - cardH) / 2;

  uint8_t displayHour = hour;
  if (use12Hour) {
    displayHour = hour % 12;
    if (displayHour == 0) displayHour = 12;
  }

  int h1 = displayHour / 10;
  int h2 = displayHour % 10;
  int m1 = minute / 10;
  int m2 = minute % 10;

  auto drawCard = [this, startY, cardW, cardH](int x, int digit) {
    // 1. Draw rounded black card
    renderer.fillRoundedRect(x, startY, cardW, cardH, 8, Color::Black);

    // 2. Draw digit in white inside card (Digit width = 60, height = 84 at blockSize = 12)
    // Centering offsets: dx = (90 - 60)/2 = 15. dy = (136 - 84)/2 = 26.
    drawDigit(x + 15, startY + 26, digit, 12, Color::White);

    // 3. Draw horizontal flip seam line
    renderer.drawLine(x, startY + cardH / 2, x + cardW, startY + cardH / 2, 2, false); // state = false (White)

    // 4. Draw clips on side borders at the seam
    renderer.fillRect(x - 2, startY + cardH / 2 - 4, 4, 8, true); // Black clip
    renderer.fillRect(x + cardW - 2, startY + cardH / 2 - 4, 4, 8, true);
  };

  // Draw H1, H2, M1, M2
  drawCard(startX, h1);
  drawCard(startX + cardW + spacing, h2);
  drawCard(startX + 2 * cardW + 2 * spacing + centerGap, m1);
  drawCard(startX + 3 * cardW + 3 * spacing + centerGap, m2);

  // UTC / Period Suffix Info
  char infoBuf[32];
  double tzOffset = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 0.25;
  snprintf(infoBuf, sizeof(infoBuf), "%s  (UTC%+0.2f)", (use12Hour ? (hour >= 12 ? "PM" : "AM") : "24H"), tzOffset);
  renderer.drawCenteredText(NOTOSANS_14_FONT_ID, startY + cardH + 30, infoBuf);
}

// Shared MM:SS block-digit layout for Stopwatch/Timer — mirrors
// drawDigitalClock's digit math intentionally (same visual weight as the
// clock faces) but without the AM/PM/UTC info line, which doesn't apply here.
void ClockActivity::drawBigTime(int contentTop, int contentHeight, int pageWidth, int minutes, int seconds) {
  int blockSize = 18;
  int digitW = 5 * blockSize;
  int digitH = 7 * blockSize;
  int spacing = 18;
  int colonW = 18;

  int totalW = 4 * digitW + 2 * spacing + colonW + 2 * spacing;
  int startX = (pageWidth - totalW) / 2;
  int startY = contentTop + (contentHeight - digitH) / 2;

  int m1 = minutes / 10;
  int m2 = minutes % 10;
  int s1 = seconds / 10;
  int s2 = seconds % 10;

  drawDigit(startX, startY, m1, blockSize, Color::Black);
  drawDigit(startX + digitW + spacing, startY, m2, blockSize, Color::Black);
  drawColon(startX + 2 * digitW + 2 * spacing, startY, blockSize, Color::Black);
  drawDigit(startX + 2 * digitW + 2 * spacing + colonW + spacing, startY, s1, blockSize, Color::Black);
  drawDigit(startX + 3 * digitW + 3 * spacing + colonW + spacing, startY, s2, blockSize, Color::Black);
}

void ClockActivity::drawStopwatch(int contentTop, int contentHeight, int pageWidth) {
  unsigned long elapsedMs = stopwatchElapsedMs;
  if (stopwatchRunning) elapsedMs += millis() - stopwatchStartMs;

  int totalSec = static_cast<int>(elapsedMs / 1000);
  const int maxSec = 99 * 60 + 59;  // block-digit layout only has 2 digits per field
  if (totalSec > maxSec) totalSec = maxSec;

  drawBigTime(contentTop, contentHeight, pageWidth, totalSec / 60, totalSec % 60);
}

void ClockActivity::drawTimer(int contentTop, int contentHeight, int pageWidth) {
  int totalSec = std::max(0, timerRemainingSec);
  const int maxSec = 99 * 60 + 59;
  if (totalSec > maxSec) totalSec = maxSec;

  drawBigTime(contentTop, contentHeight, pageWidth, totalSec / 60, totalSec % 60);
}

void ClockActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header Title
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK));

  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  const bool isStopwatch = (mode == ClockMode::Stopwatch);
  const bool isTimer = (mode == ClockMode::Timer);

  if (isStopwatch || isTimer) {
    // Stopwatch/Timer both render as plain MM:SS block digits, so — unlike
    // Analog/Digital/Flip, which are visually distinct from each other — they
    // need an explicit label to tell them apart, plus a running/paused status.
    const int subHeaderHeight = 30;
    const char* title = isStopwatch ? tr(STR_CLOCK_STOPWATCH_TITLE) : tr(STR_CLOCK_TIMER_TITLE);
    const char* status;
    if (isTimer && timerFinished) {
      status = tr(STR_CLOCK_TIME_UP);
    } else {
      bool running = isStopwatch ? stopwatchRunning : timerRunning;
      status = running ? tr(STR_CLOCK_RUNNING) : tr(STR_CLOCK_PAUSED);
    }
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, subHeaderHeight}, title, status);
    contentTop += subHeaderHeight + metrics.verticalSpacing;
  }

  const int contentHeight = contentBottom - contentTop;

  // Mode specific drawing
  switch (mode) {
    case ClockMode::Analog: {
      uint8_t h, m;
      getLocalTime(h, m);
      drawAnalogClock(metrics, contentTop, contentHeight, pageWidth, h, m);
      break;
    }
    case ClockMode::Digital: {
      uint8_t h, m;
      getLocalTime(h, m);
      drawDigitalClock(metrics, contentTop, contentHeight, pageWidth, h, m);
      break;
    }
    case ClockMode::Flip: {
      uint8_t h, m;
      getLocalTime(h, m);
      drawFlipClock(metrics, contentTop, contentHeight, pageWidth, h, m);
      break;
    }
    case ClockMode::Stopwatch:
      drawStopwatch(contentTop, contentHeight, pageWidth);
      break;
    case ClockMode::Timer:
      drawTimer(contentTop, contentHeight, pageWidth);
      break;
  }

  if (isTimer && timerFinished) {
    GUI.drawPopup(renderer, tr(STR_CLOCK_TIME_UP));
  }

  // Draw bottom hints
  const char* confirmLabel;
  if (isStopwatch) {
    confirmLabel = stopwatchRunning ? tr(STR_CLOCK_PAUSE) : (stopwatchElapsedMs > 0 ? tr(STR_CLOCK_RESUME) : tr(STR_CLOCK_START));
  } else if (isTimer) {
    if (timerFinished) {
      confirmLabel = tr(STR_CLOCK_DISMISS);
    } else if (timerRunning) {
      confirmLabel = tr(STR_CLOCK_PAUSE);
    } else if (timerRemainingSec < timerDurationSec) {
      confirmLabel = tr(STR_CLOCK_RESUME);
    } else {
      confirmLabel = tr(STR_CLOCK_START);
    }
  } else {
    confirmLabel = use12Hour ? tr(STR_CLOCK_FORMAT_24H) : tr(STR_CLOCK_FORMAT_12H);
  }

  // Prev/Next hints name the mode they'll actually switch to, not a generic
  // "prev/next mode" — Stopwatch and Timer especially need this since nothing
  // else on screen tells them apart from each other at a glance.
  const ClockMode prevMode = static_cast<ClockMode>((static_cast<int>(mode) + MODE_COUNT - 1) % MODE_COUNT);
  const ClockMode nextMode = static_cast<ClockMode>((static_cast<int>(mode) + 1) % MODE_COUNT);

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), confirmLabel, modeShortName(prevMode), modeShortName(nextMode));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
