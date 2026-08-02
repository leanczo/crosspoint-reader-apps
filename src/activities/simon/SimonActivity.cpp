#include "SimonActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdlib>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

int SimonActivity::quadrantForButton(MappedInputManager::Button button) {
  switch (button) {
    case MappedInputManager::Button::Up:
      return 0;
    case MappedInputManager::Button::Right:
      return 1;
    case MappedInputManager::Button::Down:
      return 2;
    case MappedInputManager::Button::Left:
      return 3;
    default:
      return -1;
  }
}

void SimonActivity::resetGame() {
  sequenceLength = 0;
  state = SimonState::Idle;
  showStep = 0;
  showingOn = false;
  inputIndex = 0;
  flashIndex = -1;
  phaseStartMs = millis();
}

void SimonActivity::appendRandomStep() {
  if (sequenceLength >= MAX_SEQUENCE) return;  // defensive cap, never realistically reached
  sequence[sequenceLength++] = static_cast<int8_t>(rand() % 4);
}

void SimonActivity::startShowingSequence() {
  state = SimonState::ShowingSequence;
  showStep = 0;
  showingOn = true;
  flashIndex = -1;
  phaseStartMs = millis();
}

void SimonActivity::beginNewSequence() {
  sequenceLength = 0;
  appendRandomStep();
  startShowingSequence();
}

void SimonActivity::onEnter() {
  Activity::onEnter();
  srand(millis());
  resetGame();
  requestUpdate();
}

void SimonActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const unsigned long now = millis();

  switch (state) {
    case SimonState::Idle: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        beginNewSequence();
        requestUpdate();
      }
      return;
    }

    case SimonState::GameOver: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        beginNewSequence();
        requestUpdate();
      }
      return;
    }

    case SimonState::ShowingSequence: {
      const unsigned long phaseLen = showingOn ? SEQ_ON_MS : SEQ_OFF_MS;
      if (now - phaseStartMs < phaseLen) return;

      phaseStartMs = now;
      if (showingOn) {
        showingOn = false;
      } else {
        showStep++;
        if (showStep >= sequenceLength) {
          state = SimonState::PlayerInput;
          inputIndex = 0;
          flashIndex = -1;
        } else {
          showingOn = true;
        }
      }
      requestUpdate();
      return;
    }

    case SimonState::RoundCorrect: {
      if (now - phaseStartMs < ROUND_CORRECT_MS) return;
      appendRandomStep();
      startShowingSequence();
      requestUpdate();
      return;
    }

    case SimonState::PlayerInput: {
      int pressed = -1;
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        pressed = quadrantForButton(MappedInputManager::Button::Up);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        pressed = quadrantForButton(MappedInputManager::Button::Right);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        pressed = quadrantForButton(MappedInputManager::Button::Down);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        pressed = quadrantForButton(MappedInputManager::Button::Left);
      }
      if (pressed == -1) return;

      // Stays lit (no auto-clear timer) until the next press or state change
      // redraws it — see the flashIndex comment in the header for why.
      flashIndex = pressed;

      if (pressed == sequence[inputIndex]) {
        inputIndex++;
        if (inputIndex >= sequenceLength) {
          state = SimonState::RoundCorrect;
          phaseStartMs = now;
        }
      } else {
        state = SimonState::GameOver;
      }
      requestUpdate();
      return;
    }
  }
}

void SimonActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SIMON_TITLE));

  const int subHeaderY = metrics.topPadding + metrics.headerHeight;
  char subBuf[32];
  if (state == SimonState::Idle) {
    snprintf(subBuf, sizeof(subBuf), "%s", tr(STR_SIMON_PRESS_START));
  } else {
    snprintf(subBuf, sizeof(subBuf), "%s %d", tr(STR_SIMON_ROUND_LABEL), sequenceLength);
  }
  GUI.drawSubHeader(renderer, Rect{0, subHeaderY, pageWidth, SUBHEADER_HEIGHT}, subBuf, nullptr);

  // Plus-shaped D-pad layout: Up/Right/Down/Left quadrants around an empty
  // center, so the on-screen buttons sit in the same relative positions as
  // the physical D-pad (no separate mapping for the player to learn).
  const int contentTop = subHeaderY + SUBHEADER_HEIGHT + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int availH = contentBottom - contentTop;
  const int side = std::min(pageWidth, availH);
  const int cell = side / 3;
  const int originX = (pageWidth - cell * 3) / 2;
  const int originY = contentTop + (availH - cell * 3) / 2;

  const int upX = originX + cell, upY = originY;
  const int downX = originX + cell, downY = originY + 2 * cell;
  const int leftX = originX, leftY = originY + cell;
  const int rightX = originX + 2 * cell, rightY = originY + cell;

  auto isLit = [&](int quadrant) {
    if (state == SimonState::ShowingSequence && showingOn && showStep < sequenceLength) {
      return sequence[showStep] == quadrant;
    }
    if (state == SimonState::PlayerInput && flashIndex == quadrant) {
      return true;
    }
    return false;
  };

  struct QuadrantDef {
    int x, y, idx;
    const char* label;
  };
  const QuadrantDef quads[4] = {
      {upX, upY, 0, tr(STR_SIMON_COLOR_GREEN)},
      {rightX, rightY, 1, tr(STR_SIMON_COLOR_RED)},
      {downX, downY, 2, tr(STR_SIMON_COLOR_YELLOW)},
      {leftX, leftY, 3, tr(STR_SIMON_COLOR_BLUE)},
  };

  for (const auto& q : quads) {
    const bool lit = isLit(q.idx);
    renderer.drawRoundedRect(q.x, q.y, cell, cell, 2, 10, true);
    if (lit) {
      renderer.fillRoundedRect(q.x, q.y, cell, cell, 10, Color::Black);
    }
    const int textW = renderer.getTextWidth(UI_12_FONT_ID, q.label, EpdFontFamily::BOLD);
    const int textH = renderer.getLineHeight(UI_12_FONT_ID);
    const int textX = q.x + (cell - textW) / 2;
    const int textY = q.y + (cell - textH) / 2;
    renderer.drawText(UI_12_FONT_ID, textX, textY, q.label, !lit, EpdFontFamily::BOLD);
  }

  // Quiet decorative outline in the unused center cell ties the 4 quadrants
  // together visually as a single D-pad-shaped control.
  renderer.drawRoundedRect(originX + cell, originY + cell, cell, cell, 1, 10, true);

  if (state == SimonState::GameOver) {
    char overBuf[48];
    snprintf(overBuf, sizeof(overBuf), "%s %d", tr(STR_SIMON_GAME_OVER), sequenceLength - 1);
    GUI.drawPopup(renderer, overBuf);
  } else if (state == SimonState::RoundCorrect) {
    // Explicit confirmation banner: without this, the round boundary was
    // silent and the next sequence could start flashing before the player
    // even registered that their last press was correct.
    GUI.drawPopup(renderer, tr(STR_SIMON_CORRECT));
  }

  const char* confirmLabel =
      (state == SimonState::Idle || state == SimonState::GameOver) ? tr(STR_SIMON_START) : nullptr;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, nullptr, nullptr);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
