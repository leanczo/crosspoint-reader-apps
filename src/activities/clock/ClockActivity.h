#pragma once

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"
#include <cstdint>

enum class ClockMode {
  Analog,
  Digital,
  Flip,
  Stopwatch,
  Timer
};

class ClockActivity final : public Activity {
 public:
  ClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  static constexpr int MODE_COUNT = 5;

  ClockMode mode;
  bool use12Hour;
  uint8_t lastHour;
  uint8_t lastMinute;
  unsigned long lastTimeCheck = 0;

  // Stopwatch state. Elapsed time is always derived from millis() deltas
  // (never accumulated per-tick) so it can't drift from re-render timing.
  bool stopwatchRunning = false;
  unsigned long stopwatchStartMs = 0;
  unsigned long stopwatchElapsedMs = 0;

  // Timer state. timerDurationSec is the configured duration (adjustable in
  // 1-minute steps while idle); timerRemainingSec is authoritative whenever
  // the timer is NOT running (idle or paused); timerEndMs is the absolute
  // millis() deadline while it IS running, so the countdown can't drift.
  bool timerRunning = false;
  bool timerFinished = false;
  int timerDurationSec = 5 * 60;
  int timerRemainingSec = 5 * 60;
  unsigned long timerEndMs = 0;

  ButtonNavigator buttonNavigator;

  static const char* modeShortName(ClockMode m);
  void syncClock();
  void getLocalTime(uint8_t& hour, uint8_t& minute);
  void drawAnalogClock(const ThemeMetrics& metrics, int contentTop, int contentHeight, int pageWidth, uint8_t hour, uint8_t minute);
  void drawDigitalClock(const ThemeMetrics& metrics, int contentTop, int contentHeight, int pageWidth, uint8_t hour, uint8_t minute);
  void drawFlipClock(const ThemeMetrics& metrics, int contentTop, int contentHeight, int pageWidth, uint8_t hour, uint8_t minute);
  void drawStopwatch(int contentTop, int contentHeight, int pageWidth);
  void drawTimer(int contentTop, int contentHeight, int pageWidth);
  void drawBigTime(int contentTop, int contentHeight, int pageWidth, int minutes, int seconds);

  void drawDigit(int x, int y, int digit, int blockSize, Color color);
  void drawColon(int x, int y, int blockSize, Color color);
};
