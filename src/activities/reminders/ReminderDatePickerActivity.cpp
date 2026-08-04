#include "ReminderDatePickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
const char* repeatLabelFor(DateMath::RepeatRule repeat) {
  switch (repeat) {
    case DateMath::RepeatRule::Weekly:
      return tr(STR_REPEAT_WEEKLY);
    case DateMath::RepeatRule::Monthly:
      return tr(STR_REPEAT_MONTHLY);
    case DateMath::RepeatRule::Yearly:
      return tr(STR_REPEAT_YEARLY);
    case DateMath::RepeatRule::Never:
    default:
      return tr(STR_REPEAT_NEVER);
  }
}
}  // namespace

void ReminderDatePickerActivity::onEnter() {
  Activity::onEnter();
  activeField = FIELD_DAY;
  requestUpdate();
}

void ReminderDatePickerActivity::clampDayField() {
  day = static_cast<uint8_t>(DateMath::clampDay(year, month, day));
}

void ReminderDatePickerActivity::adjustActiveField(int delta) {
  switch (activeField) {
    case FIELD_DAY: {
      const int total = DateMath::daysInMonth(year, month);
      const int next = ((static_cast<int>(day) - 1 + delta) % total + total) % total + 1;
      day = static_cast<uint8_t>(next);
      break;
    }
    case FIELD_MONTH: {
      const int next = ((static_cast<int>(month) - 1 + delta) % 12 + 12) % 12 + 1;
      month = static_cast<uint8_t>(next);
      clampDayField();
      break;
    }
    case FIELD_YEAR: {
      constexpr int range = MAX_YEAR - MIN_YEAR + 1;
      const int next = ((static_cast<int>(year) - MIN_YEAR + delta) % range + range) % range + MIN_YEAR;
      year = static_cast<int16_t>(next);
      clampDayField();
      break;
    }
    case FIELD_REPEAT: {
      const int next = ((static_cast<int>(repeat) + delta) % 4 + 4) % 4;
      repeat = static_cast<DateMath::RepeatRule>(next);
      break;
    }
    default:
      break;
  }
}

void ReminderDatePickerActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    setResult(ReminderDateResult{year, month, day, repeat});
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activeField = static_cast<Field>((activeField + 1) % FIELD_COUNT);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    adjustActiveField(+1);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    adjustActiveField(-1);
    requestUpdate();
  });
}

void ReminderDatePickerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                tr(STR_REMINDER_DATE_REPEAT));

  // Date shown numerically (DD/MM/YYYY), not with month names, since this
  // codebase's month-name table (CalendarActivity) is hardcoded English with
  // no i18n - numeric avoids introducing that same gap here.
  char dateBuf[16];
  snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%04d", day, month, year);
  const char* repeatLabel = repeatLabelFor(repeat);

  const int dateY = pageHeight / 2 - 40;
  const int repeatY = dateY + 60;

  renderer.drawCenteredText(UI_12_FONT_ID, dateY, dateBuf, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, repeatY, repeatLabel, true, EpdFontFamily::BOLD);

  auto widthOf = [&](const char* s) { return renderer.getTextWidth(UI_12_FONT_ID, s); };

  if (activeField == FIELD_REPEAT) {
    const int totalWidth = widthOf(repeatLabel);
    const int leftEdge = (pageWidth - totalWidth) / 2;
    const int caretY = repeatY + 10;
    for (int dy = 0; dy < 2; dy++) {
      renderer.drawLine(leftEdge, caretY + dy, leftEdge + totalWidth, caretY + dy);
    }
  } else {
    const int totalWidth = widthOf(dateBuf);
    const int leftEdge = (pageWidth - totalWidth) / 2;

    char dayStr[4];
    snprintf(dayStr, sizeof(dayStr), "%02d", day);
    char prefixMonth[8];
    snprintf(prefixMonth, sizeof(prefixMonth), "%02d/", day);
    char monthStr[4];
    snprintf(monthStr, sizeof(monthStr), "%02d", month);
    char prefixYear[16];
    snprintf(prefixYear, sizeof(prefixYear), "%02d/%02d/", day, month);
    char yearStr[8];
    snprintf(yearStr, sizeof(yearStr), "%04d", year);

    int caretX = leftEdge;
    int caretW = 0;
    switch (activeField) {
      case FIELD_DAY:
        caretX = leftEdge;
        caretW = widthOf(dayStr);
        break;
      case FIELD_MONTH:
        caretX = leftEdge + widthOf(prefixMonth);
        caretW = widthOf(monthStr);
        break;
      case FIELD_YEAR:
        caretX = leftEdge + widthOf(prefixYear);
        caretW = widthOf(yearStr);
        break;
      default:
        break;
    }
    const int caretY = dateY + 10;
    for (int dy = 0; dy < 2; dy++) {
      renderer.drawLine(caretX, caretY + dy, caretX + caretW, caretY + dy);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEXT_FIELD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, ButtonArrow::Up,
                      ButtonArrow::Down);

  renderer.displayBuffer();
}
