#include "KeyboardEntryActivity.h"

#include <HalGPIO.h>
#include <I18n.h>
#include <ScriptDetector.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// The edit buffer is bytes; Arabic characters are two of them. Every place that used
// to step one byte has to step one character instead, or Del leaves half a letter
// behind and the cursor lands inside a sequence.
bool isUtf8Continuation(const char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// First byte of the character ending at `pos`.
size_t utf8PrevStart(const std::string& s, size_t pos) {
  if (pos == 0) return 0;
  size_t i = pos - 1;
  while (i > 0 && isUtf8Continuation(s[i])) i--;
  return i;
}

// One past the last byte of the character starting at `pos`.
size_t utf8NextEnd(const std::string& s, size_t pos) {
  if (pos >= s.size()) return s.size();
  size_t i = pos + 1;
  while (i < s.size() && isUtf8Continuation(s[i])) i++;
  return i;
}

// The whole character at `pos`, for measuring and highlighting the cursor cell.
std::string utf8CharAt(const std::string& s, size_t pos) {
  if (pos >= s.size()) return {};
  return s.substr(pos, utf8NextEnd(s, pos) - pos);
}
}  // namespace

const char* const KeyboardEntryActivity::shiftString[2] = {"shift", "SHIFT"};

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();
  cursorPos = text.length();
  symMode = false;
  // Search opens straight into Arabic for a reader whose interface is Arabic --
  // see preferArabic. Everything else starts on abc, because a WiFi password or an
  // SSID is not going to be Arabic no matter what language the menus are in.
  arabicMode = preferArabic && arabicPanelAvailable();
  urlMode = false;
  cursorMode = false;
  togglePos = false;
  passwordVisible = false;
  shiftState = 0;
  selectedRow = 0;
  selectedCol = 0;
  delPressCount = 0;
  hintVisible = false;
  hintShowTime = 0;
  rightHeld = false;
  rightLongHandled = false;
  savedCursorPos = 0;
  rightStartCursorPos = 0;
  requestUpdate();
}

void KeyboardEntryActivity::onExit() { Activity::onExit(); }

int KeyboardEntryActivity::getContentRowCount() const {
  if (urlMode) return 3;
  if (numericOnly) return (NUMERIC_KEY_COUNT + NUMERIC_COLS - 1) / NUMERIC_COLS;
  return ABC_ROWS;
}

int KeyboardEntryActivity::getContentColCount() const {
  if (urlMode) return 3;
  if (numericOnly) return NUMERIC_COLS;
  return COLS;
}

int KeyboardEntryActivity::getTotalRowCount() const { return getContentRowCount() + 1; }

bool KeyboardEntryActivity::isBottomRow(const int row) const { return row == getContentRowCount(); }

char KeyboardEntryActivity::getSelectedChar() const {
  if (numericOnly) {
    const int idx = selectedCol + selectedRow * NUMERIC_COLS;
    return (idx >= 0 && idx < NUMERIC_KEY_COUNT) ? numericKeys[idx] : '\0';
  }

  const KeyDef(*layout)[COLS] = symMode ? symLayout : (inputType == InputType::Url ? urlLayout : abcLayout);

  if (selectedRow < 0 || selectedRow >= getContentRowCount()) return '\0';
  if (selectedCol < 0 || selectedCol >= COLS) return '\0';

  const KeyDef& key = layout[selectedRow][selectedCol];
  return (shiftState > 0 && key.secondary != '\0') ? key.secondary : key.primary;
}

char KeyboardEntryActivity::getAlternativeChar() const {
  if (symMode || arabicMode || urlMode || numericOnly) return '\0';
  if (inputType == InputType::Url && selectedRow > 0) return '\0';

  const KeyDef(*layout)[COLS] = abcLayout;

  if (selectedRow < 0 || selectedRow >= getContentRowCount()) return '\0';
  if (selectedCol < 0 || selectedCol >= COLS) return '\0';

  const KeyDef& key = layout[selectedRow][selectedCol];
  const char current = getSelectedChar();
  if (current == key.primary && key.secondary != '\0') return key.secondary;
  if (current == key.secondary) return key.primary;
  return '\0';
}

bool KeyboardEntryActivity::arabicPanelAvailable() const { return !numericOnly && inputType == InputType::Text; }

std::string KeyboardEntryActivity::getSelectedKeyText() const {
  if (arabicMode) {
    if (selectedRow < 0 || selectedRow >= ARA_ROWS) return {};
    if (selectedCol < 0 || selectedCol >= COLS) return {};
    return arabicLayout[selectedRow][selectedCol];
  }
  const char c = getSelectedChar();
  return c == '\0' ? std::string{} : std::string(1, c);
}

bool KeyboardEntryActivity::insertChar(char c) {
  if (c == '\0') return true;
  if (maxLength != 0 && text.length() >= maxLength) return true;
  if (cursorPos > text.length()) cursorPos = text.length();

  text.insert(cursorPos, 1, c);
  cursorPos++;
  return true;
}

void KeyboardEntryActivity::insertString(const std::string& str) {
  if (str.empty()) return;
  if (maxLength != 0 && text.length() + str.length() > maxLength) return;
  if (cursorPos > text.length()) cursorPos = text.length();

  text.insert(cursorPos, str);
  cursorPos += str.length();
}

bool KeyboardEntryActivity::handleKeyPress() {
  if (isBottomRow(selectedRow)) {
    switch (static_cast<SpecialKeyType>(selectedCol)) {
      case SpecialKeyType::Shift:
        delPressCount = 0;
        hintVisible = false;
        // Shift is meaningless in the URL-snippet panel, the symbol layout, and the
        // digit-only numeric panel, but must work for the URL letter layout so uppercase
        // letters can be entered (#2178).
        if (urlMode) return true;
        if (symMode) return true;
        if (arabicMode) return true;  // Arabic has no case
        if (numericOnly) return true;
        shiftState = (shiftState + 1) % 2;
        return true;
      case SpecialKeyType::Mode: {
        delPressCount = 0;
        hintVisible = false;
        if (numericOnly) return true;  // no other panel to switch to
        if (urlMode) {
          urlMode = false;
          symMode = false;
          selectedRow = getTotalRowCount() - 1;
          selectedCol = static_cast<int>(SpecialKeyType::Mode);
          requestUpdate();
          return true;
        }
        // Symbols on, symbols off. Nothing else -- changing writing system is the
        // Lang key's job.
        symMode = !symMode;
        if (symMode) arabicMode = false;
        int maxRow = getTotalRowCount() - 1;
        if (selectedRow > maxRow) selectedRow = maxRow;
        if (isBottomRow(selectedRow)) {
          if (selectedCol >= BOTTOM_KEY_COUNT) selectedCol = BOTTOM_KEY_COUNT - 1;
        } else {
          if (selectedCol >= getContentColCount()) selectedCol = getContentColCount() - 1;
        }
        return true;
      }
      case SpecialKeyType::Lang: {
        delPressCount = 0;
        hintVisible = false;
        if (!arabicPanelAvailable()) return true;
        arabicMode = !arabicMode;
        if (arabicMode) symMode = false;
        const int maxRow = getTotalRowCount() - 1;
        if (selectedRow > maxRow) selectedRow = maxRow;
        return true;
      }
      case SpecialKeyType::Space:
        delPressCount = 0;
        hintVisible = false;
        if (numericOnly) {
          return true;  // no spaces in a digit-only field
        } else if (inputType == InputType::Url) {
          urlMode = !urlMode;
          if (urlMode) {
            symMode = false;
          }
          selectedRow = getTotalRowCount() - 1;
          selectedCol = static_cast<int>(SpecialKeyType::Space);
          requestUpdate();
        } else {
          return insertChar(' ');
        }
        return true;
      case SpecialKeyType::Del:
        delPressCount++;
        if (delPressCount >= 2) {
          hintVisible = true;
          hintShowTime = millis();
        }
        if (cursorPos > 0 && !text.empty()) {
          const size_t start = utf8PrevStart(text, cursorPos);
          text.erase(start, cursorPos - start);
          cursorPos = start;
        }
        return true;
      case SpecialKeyType::Ok:
        delPressCount = 0;
        hintVisible = false;
        onComplete(text);
        return false;
      default:
        return true;
    }
  }

  if (urlMode) {
    delPressCount = 0;
    hintVisible = false;
    const int idx = selectedCol + selectedRow * 3;
    if (idx < URL_SNIPPET_COUNT) {
      insertString(urlSnippets[idx]);
    }
    return true;
  }

  delPressCount = 0;
  hintVisible = false;

  insertString(getSelectedKeyText());
  return true;
}

void KeyboardEntryActivity::mapColContentBottom(int& col, bool goingUp) const {
  if (urlMode || numericOnly) {
    col = goingUp ? col - 1 : col + 1;
    if (col < 0) col = 0;
    if (col >= 3) col = 2;
  } else {
    col = goingUp ? col * COLS / BOTTOM_KEY_COUNT : col * BOTTOM_KEY_COUNT / COLS;
  }
}

void KeyboardEntryActivity::loop() {
  const int totalRows = getTotalRowCount();

  if (!cursorMode && mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    upHeld = true;
    upLongHandled = false;
  }

  if (upHeld && !upLongHandled && mappedInput.isPressed(MappedInputManager::Button::Up) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    cursorMode = true;
    upLongHandled = true;
    hintVisible = true;
    hintShowTime = millis();
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (upHeld && !upLongHandled && !cursorMode) {
      bool wasBottom = isBottomRow(selectedRow);
      const int contentCols = getContentColCount();
      selectedRow = ButtonNavigator::previousIndex(selectedRow, totalRows);
      if (wasBottom && !isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, true);
      } else if (!wasBottom && isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, false);
      }
      int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : contentCols - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
      requestUpdate();
    }
    upHeld = false;
    upLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    downHeld = true;
    if (cursorMode) {
      togglePos = false;
      passwordVisible = false;
      cursorMode = false;
      hintVisible = false;
      downLongHandled = true;
      requestUpdate();
    } else {
      downLongHandled = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (downHeld && !downLongHandled && !cursorMode) {
      bool wasBottom = isBottomRow(selectedRow);
      const int contentCols = getContentColCount();
      selectedRow = ButtonNavigator::nextIndex(selectedRow, totalRows);
      if (wasBottom && !isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, true);
      } else if (!wasBottom && isBottomRow(selectedRow)) {
        mapColContentBottom(selectedCol, false);
      }
      int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : contentCols - 1;
      if (selectedCol > maxCol) selectedCol = maxCol;
      requestUpdate();
    }
    downHeld = false;
    downLongHandled = false;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    if (cursorMode) return;
    int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : getContentColCount() - 1;
    selectedCol = ButtonNavigator::previousIndex(selectedCol, maxCol + 1);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (cursorMode) {
      if (togglePos) {
        cursorPos = savedCursorPos;
        togglePos = false;
        requestUpdate();
      } else if (cursorPos > 0) {
        cursorPos = utf8PrevStart(text, cursorPos);
        requestUpdate();
      }
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (cursorMode && inputType == InputType::Password && !togglePos) {
      rightHeld = true;
      rightLongHandled = false;
      rightStartCursorPos = cursorPos;
    }
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] {
    if (cursorMode) return;
    int maxCol = isBottomRow(selectedRow) ? BOTTOM_KEY_COUNT - 1 : getContentColCount() - 1;
    selectedCol = ButtonNavigator::nextIndex(selectedCol, maxCol + 1);
    requestUpdate();
  });

  if (rightHeld && !rightLongHandled && mappedInput.isPressed(MappedInputManager::Button::Right) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    if (cursorMode && inputType == InputType::Password && !togglePos) {
      savedCursorPos = rightStartCursorPos;
      togglePos = true;
      rightLongHandled = true;
      requestUpdate();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (cursorMode && inputType == InputType::Password) {
      rightHeld = false;
      rightLongHandled = false;
    }
    if (cursorMode && !togglePos && cursorPos < text.length()) {
      cursorPos = utf8NextEnd(text, cursorPos);
      requestUpdate();
    }
    if (cursorMode) return;
    rightHeld = false;
    rightLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmHeld = true;
    confirmLongHandled = false;
  }

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > DEL_LONG_PRESS_MS && isBottomRow(selectedRow) &&
      selectedCol == static_cast<int>(SpecialKeyType::Del)) {
    text.clear();
    cursorPos = 0;
    confirmLongHandled = true;
    requestUpdate();
  }

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    char alt = getAlternativeChar();
    if (alt != '\0') {
      insertChar(alt);
      requestUpdate();
      confirmLongHandled = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmHeld && !confirmLongHandled && !cursorMode) {
      if (handleKeyPress()) {
        requestUpdate();
      }
    } else if (confirmHeld && !confirmLongHandled && cursorMode && inputType == InputType::Password && togglePos) {
      passwordVisible = !passwordVisible;
      requestUpdate();
    }
    confirmHeld = false;
    confirmLongHandled = false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onCancel();
  }

  if (hintVisible && !cursorMode && millis() - hintShowTime > 4000) {
    hintVisible = false;
    requestUpdate();
  }
}

void KeyboardEntryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str());

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int inputStartY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing +
                          metrics.verticalSpacing * 4 + metrics.keyboardVerticalOffset;
  int inputHeight = 0;

  std::string displayText;
  if (inputType == InputType::Password && !passwordVisible) {
    size_t revealPos;
    if (cursorMode) {
      revealPos = text.length();  // no reveal in displayText; block draws actual char directly
    } else {
      revealPos = (text.length() > 0 && cursorPos > 0) ? cursorPos - 1 : std::string::npos;
    }
    displayText = text;
    for (size_t i = 0; i < displayText.length(); i++) {
      if (i != revealPos) {
        displayText[i] = '*';
      }
    }
  } else {
    displayText = text;
  }

  const bool isPassword = (inputType == InputType::Password);
  int availableWidth = pageWidth;
  if (gpio.deviceIsX3()) {
    availableWidth -= 2 * metrics.sideButtonHintsWidth;
  }
  const int effectiveMargin = (pageWidth - availableWidth * metrics.keyboardTextFieldWidthPercent / 100) / 2;
  const int toggleGap = isPassword ? 4 : 0;
  const int toggleReserve = isPassword ? std::max(renderer.getTextWidth(UI_12_FONT_ID, "[abc]"),
                                                  renderer.getTextWidth(UI_12_FONT_ID, "[***]")) +
                                             toggleGap
                                       : 0;
  const int textAreaWidth = pageWidth - 2 * effectiveMargin - toggleReserve;
  const int maxLineWidth = textAreaWidth;
  const bool centerText = metrics.keyboardCenteredText;

  int cursorCharWidth = 6;
  if (cursorPos < text.length()) {
    int w = renderer.getTextWidth(UI_12_FONT_ID, utf8CharAt(text, cursorPos).c_str());
    if (w > cursorCharWidth) cursorCharWidth = w;
  }

  // A right-to-left field: text hugs the right margin and the caret sits at the
  // LEFT end, because that is where the next letter goes. drawText already shapes
  // and reorders the glyphs -- what it cannot know is where the field's origin
  // should be, which is a layout decision and belongs here.
  //
  // Keyed on the content, not just the panel, so a line typed in Arabic keeps its
  // direction after switching back to abc to add a digit. The empty-field case
  // falls back to the panel, so opening Arabic puts the caret where the writing
  // will actually start.
  const bool rtlField = ScriptDetector::containsArabic(displayText.c_str()) || (arabicMode && displayText.empty());

  int lineStartIdx = 0;
  int lineEndIdx = displayText.length();
  int textWidth = 0;
  int cursorPixelX = effectiveMargin;
  int cursorLineY = inputStartY;
  bool cursorDrawn = false;

  while (true) {
    std::string lineText = displayText.substr(lineStartIdx, lineEndIdx - lineStartIdx);
    textWidth = renderer.getTextAdvanceX(UI_12_FONT_ID, lineText.c_str(), EpdFontFamily::REGULAR);
    if (textWidth <= maxLineWidth) {
      const bool isLastLine = (lineEndIdx == static_cast<int>(displayText.length()));
      bool isCursorLine = false;
      if (!cursorDrawn && cursorPos >= lineStartIdx &&
          (isLastLine ? cursorPos <= lineEndIdx : cursorPos < lineEndIdx)) {
        std::string beforeCursor;
        if (isPassword && !passwordVisible && cursorMode) {
          beforeCursor = std::string(cursorPos - lineStartIdx, '*');
        } else {
          beforeCursor = displayText.substr(lineStartIdx, cursorPos - lineStartIdx);
        }
        int beforeWidth = renderer.getTextAdvanceX(UI_12_FONT_ID, beforeCursor.c_str(), EpdFontFamily::REGULAR);
        int kernOffset = 0;
        if (cursorPos < displayText.length()) {
          std::string beforeAndCursor = beforeCursor + displayText.substr(cursorPos, 1);
          int beforeAndCursorWidth =
              renderer.getTextAdvanceX(UI_12_FONT_ID, beforeAndCursor.c_str(), EpdFontFamily::REGULAR);
          int charAdvance = renderer.getTextAdvanceX(UI_12_FONT_ID, utf8CharAt(displayText, cursorPos).c_str(),
                                                     EpdFontFamily::REGULAR);
          kernOffset = beforeAndCursorWidth - beforeWidth - charAdvance;
        }
        if (centerText) {
          cursorPixelX = effectiveMargin + (maxLineWidth - textWidth) / 2 + beforeWidth + kernOffset;
        } else if (rtlField) {
          // Mirrored: the prefix occupies the RIGHT side of the run, so the caret sits
          // that far in from the right edge rather than from the left.
          cursorPixelX = effectiveMargin + maxLineWidth - beforeWidth - kernOffset;
        } else {
          cursorPixelX = effectiveMargin + beforeWidth + kernOffset;
        }
        cursorLineY = inputStartY + inputHeight;
        cursorDrawn = true;
        isCursorLine = true;
      }

      const int lineStartX = centerText ? effectiveMargin + (maxLineWidth - textWidth) / 2
                             : rtlField ? effectiveMargin + maxLineWidth - textWidth
                                        : effectiveMargin;
      if (isCursorLine && cursorMode && isPassword && !passwordVisible && !togglePos) {
        // Draw text in 3 parts to avoid block cursor overflowing onto next char.
        // displayText uses '*' for all chars; actual char may be wider than '*'.
        // Part 1: chars before cursor position
        const std::string part1 = displayText.substr(lineStartIdx, cursorPos - lineStartIdx);
        renderer.drawText(UI_12_FONT_ID, lineStartX, inputStartY + inputHeight, part1.c_str());
        // Part 2: skip cursor slot (block + actual char drawn later)
        // Part 3: chars after cursor position (skip char under cursor), starting at cursorPixelX + cursorCharWidth
        const int afterStart = static_cast<int>(cursorPos) + (cursorPos < text.length() ? 1 : 0);
        const int afterEnd = lineEndIdx;
        if (afterStart < afterEnd) {
          const std::string part3 = displayText.substr(afterStart, afterEnd - afterStart);
          renderer.drawText(UI_12_FONT_ID, cursorPixelX + cursorCharWidth, inputStartY + inputHeight, part3.c_str());
        }
      } else {
        renderer.drawText(UI_12_FONT_ID, lineStartX, inputStartY + inputHeight, lineText.c_str());
      }
      if (lineEndIdx == displayText.length()) {
        break;
      }

      inputHeight += lineHeight;
      lineStartIdx = lineEndIdx;
      lineEndIdx = displayText.length();
    } else {
      lineEndIdx -= 1;
    }
  }

  const int fieldWidth = (inputHeight > 0) ? maxLineWidth : textWidth;
  const int lineMargin = effectiveMargin;
  GUI.drawTextField(renderer, Rect{0, inputStartY, pageWidth, inputHeight}, fieldWidth, cursorMode, lineMargin,
                    pageWidth - 2 * lineMargin);

  if (cursorMode && !togglePos && cursorPos <= displayText.length()) {
    static constexpr int blockPadding = 1;
    renderer.fillRect(cursorPixelX - blockPadding, cursorLineY, cursorCharWidth + blockPadding * 2, lineHeight, true);
    if (cursorPos < text.length()) {
      const char buf[2] = {text[cursorPos], '\0'};
      renderer.drawText(UI_12_FONT_ID, cursorPixelX, cursorLineY, buf, false);
    }
  } else if (cursorPos <= displayText.length()) {
    static constexpr int serifW = 3;
    const int cX = cursorPixelX;
    const int cY = cursorLineY;
    const int cBottom = cursorLineY + lineHeight - 1;
    renderer.fillRect(cX, cY, 2, lineHeight, true);
    renderer.drawLine(cX - serifW, cY, cX - 1, cY, 2, true);
    renderer.drawLine(cX + 1, cY, cX + serifW, cY, 2, true);
    renderer.drawLine(cX - serifW, cBottom, cX - 1, cBottom, 2, true);
    renderer.drawLine(cX + 1, cBottom, cX + serifW, cBottom, 2, true);
  }

  if (isPassword) {
    const char* toggleLabel = passwordVisible ? "[***]" : "[abc]";
    const int toggleWidth = renderer.getTextWidth(UI_12_FONT_ID, toggleLabel);
    const int toggleX = pageWidth - effectiveMargin - toggleWidth;
    const int toggleY = inputStartY + inputHeight;
    const bool toggleSelected = cursorMode && togglePos;

    if (toggleSelected) {
      renderer.fillRect(toggleX - 2, toggleY, toggleWidth + 5, lineHeight + 3, true);
      renderer.drawText(UI_12_FONT_ID, toggleX, toggleY, toggleLabel, false);
    } else {
      renderer.drawText(UI_12_FONT_ID, toggleX, toggleY, toggleLabel, true);
    }
  }

  if (hintVisible && !text.empty()) {
    const int hintLh = renderer.getLineHeight(SMALL_FONT_ID);
    const int underlineY = inputStartY + inputHeight + lineHeight + metrics.verticalSpacing;
    const int hintY = underlineY + 4;
    if (cursorMode) {
      int hintLineY = hintY;
      if (inputType == InputType::Password && togglePos) {
        renderer.drawCenteredText(
            SMALL_FONT_ID, hintLineY,
            passwordVisible ? tr(STR_KB_HINT_TOGGLE_HIDE_PASSWORD) : tr(STR_KB_HINT_TOGGLE_SHOW_PASSWORD), true);
        hintLineY += hintLh;
        renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, tr(STR_KB_HINT_RETURN_CURSOR), true);
      } else {
        renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, tr(STR_KB_HINT_MOVE_CURSOR), true);
        hintLineY += hintLh;
        if (inputType == InputType::Password) {
          const char* passTip = passwordVisible ? tr(STR_KB_HINT_HIDE_PASSWORD) : tr(STR_KB_HINT_SHOW_PASSWORD);
          renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, passTip, true);
        }
      }
    } else {
      renderer.drawCenteredText(SMALL_FONT_ID, hintY, tr(STR_KB_HINT_EDIT_ENTRY), true);
    }
  }

  const int keyHeight = metrics.keyboardKeyHeight;
  const int bottomKeyHeight = metrics.keyboardBottomKeyHeight;
  const int keySpacing = metrics.keyboardKeySpacing;
  const int contentCols = getContentColCount();
  const int keyboardWidth = pageWidth * metrics.keyboardWidthPercent / 100;
  const int keyWidth = (keyboardWidth - (contentCols - 1) * keySpacing) / contentCols;
  const int leftMargin = (pageWidth - (contentCols * keyWidth + (contentCols - 1) * keySpacing)) / 2;

  const int bottomRowGap = metrics.keyboardBottomKeySpacing > 0 ? 4 : 0;
  const int keyboardStartY = metrics.keyboardBottomAligned
                                 ? pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing -
                                       (keyHeight + keySpacing) * getContentRowCount() - bottomKeyHeight -
                                       bottomRowGap + metrics.keyboardVerticalOffset
                                 : inputStartY + inputHeight + lineHeight + metrics.verticalSpacing;

  const int tipsLh = renderer.getLineHeight(SMALL_FONT_ID);
  const int underlineBottom = inputStartY + inputHeight + lineHeight + metrics.verticalSpacing + 4;
  auto drawTip = [&](const char* tip, int y) { renderer.drawCenteredText(SMALL_FONT_ID, y, tip, true); };

  int tipCount = 0;
  if (cursorMode) {
    tipCount = 1;
  } else if (!customTip.empty()) {
    // Caller-supplied context (e.g. Foulad eBooks login) replaces the normal
    // mode-dependent hints entirely, rather than trying to merge with them.
    tipCount = 1;
  } else if (urlMode) {
    tipCount = 1 + (!text.empty() ? 1 : 0);
  } else if (symMode) {
    tipCount = !text.empty() ? 1 : 0;
  } else {
    tipCount = 1 + (inputType == InputType::Url ? 1 : 0) + (!text.empty() ? 1 : 0);
  }

  if (tipCount > 0) {
    int y = (underlineBottom + keyboardStartY) / 2 - (tipCount + 1) * tipsLh / 2;
    drawTip(tr(STR_KB_TIPS), y);
    y += tipsLh;
    if (cursorMode) {
      drawTip(tr(STR_KB_HINT_RETURN_KEYBOARD), y);
    } else if (!customTip.empty()) {
      drawTip(customTip.c_str(), y);
    } else if (urlMode) {
      drawTip(tr(STR_KB_HINT_EXIT_URL_MODE), y);
      y += tipsLh;
      if (!text.empty()) {
        drawTip(tr(STR_KB_HINT_CLEAR_TEXT), y);
      }
    } else if (symMode || arabicMode) {
      // Neither panel has a shifted or secondary character, so the "hold SELECT for
      // UPPERCASE" tip below would be advertising a key that does nothing.
      //
      // The way out is worth stating here: someone who reached a panel by pressing an
      // unlabelled-looking key should not have to guess that the same key keeps going.
      if (arabicMode) {
        drawTip(tr(STR_KB_HINT_SWITCH_PANEL), y);
        y += tipsLh;
      }
      if (!text.empty()) {
        drawTip(tr(STR_KB_HINT_CLEAR_TEXT), y);
      }
    } else {
      const char* altCharTip;
      if (inputType == InputType::Url) {
        altCharTip = tr(STR_KB_HINT_SECONDARY_CHAR);
      } else if (shiftState > 0) {
        altCharTip = tr(STR_KB_HINT_LOWER_SECONDARY);
      } else {
        altCharTip = tr(STR_KB_HINT_UPPER_SECONDARY);
      }
      drawTip(altCharTip, y);
      y += tipsLh;
      // Says how to reach the Arabic panel, on the panel you would be looking for it
      // from. The key is one press away and labelled, but a label alone was what the
      // hidden search binding relied on and that was not enough either.
      if (arabicPanelAvailable()) {
        drawTip(tr(STR_KB_HINT_ARABIC_PANEL), y);
        y += tipsLh;
      }
      if (inputType == InputType::Url) {
        drawTip(tr(STR_KB_HINT_URL_SNIPPETS), y);
        y += tipsLh;
      }
      if (!text.empty()) {
        drawTip(tr(STR_KB_HINT_CLEAR_TEXT), y);
      }
    }
  }

  const int bkSpacing = metrics.keyboardBottomKeySpacing;
  const int abcKeyWidth = (keyboardWidth - (COLS - 1) * keySpacing) / COLS;
  const int contentTotalWidth = COLS * abcKeyWidth + (COLS - 1) * keySpacing;
  const int bottomKeyWidth = (contentTotalWidth - (BOTTOM_KEY_COUNT - 1) * bkSpacing) / BOTTOM_KEY_COUNT;
  const int bottomLeftMargin =
      (pageWidth - (BOTTOM_KEY_COUNT * bottomKeyWidth + (BOTTOM_KEY_COUNT - 1) * bkSpacing)) / 2;

  int urlLeftMargin = leftMargin;
  if (urlMode) {
    const int urlTotalWidth = 3 * keyWidth + 2 * keySpacing;
    const int urlCenterX =
        bottomLeftMargin + static_cast<int>(SpecialKeyType::Space) * (bottomKeyWidth + bkSpacing) + bottomKeyWidth / 2;
    urlLeftMargin = urlCenterX - urlTotalWidth / 2;
  }

  const KeyDef(*layout)[COLS] = symMode ? symLayout : (inputType == InputType::Url ? urlLayout : abcLayout);
  const int contentRows = getContentRowCount();

  for (int row = 0; row < contentRows; row++) {
    const int rowY = keyboardStartY + row * (keyHeight + keySpacing);
    const int rowLeftMargin = urlMode ? urlLeftMargin : leftMargin;

    for (int col = 0; col < contentCols; col++) {
      const int keyX = rowLeftMargin + col * (keyWidth + keySpacing);
      const bool isSelected = row == selectedRow && col == selectedCol;
      const bool activeKeySelected = isSelected && !cursorMode;

      if (urlMode) {
        const int snippetIdx = col + row * 3;
        if (snippetIdx < URL_SNIPPET_COUNT) {
          GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, urlSnippets[snippetIdx],
                              activeKeySelected, nullptr);
        }
      } else if (numericOnly) {
        const int idx = col + row * NUMERIC_COLS;
        if (idx < NUMERIC_KEY_COUNT) {
          const char digitBuf[2] = {numericKeys[idx], '\0'};
          GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, digitBuf, activeKeySelected, nullptr);
        }
      } else if (arabicMode) {
        // drawText routes anything containing Arabic through the shaper and the
        // built-in Noto Sans Arabic, so an isolated letter on a key needs nothing
        // special here -- isolated is exactly the form a key should show.
        GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, arabicLayout[row][col], activeKeySelected,
                            nullptr);
      } else {
        const KeyDef& key = layout[row][col];

        char primaryChar = key.primary;
        char secondaryChar = key.secondary;

        if (!symMode && shiftState > 0 && key.secondary != '\0') {
          primaryChar = key.secondary;
          secondaryChar = key.primary;
        }

        const char primaryBuf[2] = {primaryChar, '\0'};
        const char secondaryBuf[2] = {secondaryChar, '\0'};
        const bool showSecondary = !symMode && row == 0 && secondaryChar != '\0';
        GUI.drawKeyboardKey(renderer, Rect{keyX, rowY, keyWidth, keyHeight}, primaryBuf, activeKeySelected,
                            showSecondary ? secondaryBuf : nullptr);
      }
    }
  }

  const int bottomRowY = keyboardStartY + contentRows * (keyHeight + keySpacing) + bottomRowGap;
  const bool bottomSelected = isBottomRow(selectedRow);

  struct BottomKeyInfo {
    KeyboardKeyType themeType;
    const char* label;
  };
  const BottomKeyInfo bottomKeys[BOTTOM_KEY_COUNT] = {
      {(symMode || arabicMode || urlMode || numericOnly) ? KeyboardKeyType::Disabled : KeyboardKeyType::Shift,
       (symMode || arabicMode || urlMode || numericOnly) ? shiftString[0] : shiftString[shiftState]},
      {numericOnly ? KeyboardKeyType::Disabled : KeyboardKeyType::Mode,
       // Each key names the one thing it does, and does it in both directions.
       urlMode       ? "abc"
       : symMode     ? "abc"
       : numericOnly ? ""
                     : "#@!"},
      {arabicPanelAvailable() ? KeyboardKeyType::Mode : KeyboardKeyType::Disabled,
       !arabicPanelAvailable() ? "" : (arabicMode ? "abc" : "ع")},
      {numericOnly                   ? KeyboardKeyType::Disabled
       : inputType == InputType::Url ? KeyboardKeyType::Mode
                                     : KeyboardKeyType::Space,
       inputType == InputType::Url ? "URL" : nullptr},
      {KeyboardKeyType::Del, nullptr},
      {KeyboardKeyType::Ok, tr(STR_OK_BUTTON)},
  };

  for (int i = 0; i < BOTTOM_KEY_COUNT; i++) {
    const int keyX = bottomLeftMargin + i * (bottomKeyWidth + bkSpacing);
    const bool isSelected = bottomSelected && i == selectedCol;

    const bool activeKeySelected = isSelected && !cursorMode;
    GUI.drawKeyboardKey(renderer, Rect{keyX, bottomRowY, bottomKeyWidth, bottomKeyHeight}, bottomKeys[i].label,
                        activeKeySelected, nullptr, bottomKeys[i].themeType);
  }

  if (cursorMode) {
    int selKeyX, selKeyY, selKeyW, selKeyH;
    if (isBottomRow(selectedRow)) {
      selKeyX = bottomLeftMargin + selectedCol * (bottomKeyWidth + bkSpacing);
      selKeyY = bottomRowY;
      selKeyW = bottomKeyWidth;
      selKeyH = bottomKeyHeight;
    } else {
      const int rowLM = urlMode ? urlLeftMargin : leftMargin;
      selKeyX = rowLM + selectedCol * (keyWidth + keySpacing);
      selKeyY = keyboardStartY + selectedRow * (keyHeight + keySpacing);
      selKeyW = keyWidth;
      selKeyH = keyHeight;
    }
    if (isBottomRow(selectedRow)) {
      GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, bottomKeys[selectedCol].label, true,
                          nullptr, bottomKeys[selectedCol].themeType, true);
    } else if (urlMode) {
      const int idx = selectedCol + selectedRow * 3;
      if (idx < URL_SNIPPET_COUNT) {
        GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, urlSnippets[idx], true, nullptr,
                            KeyboardKeyType::Normal, true);
      }
    } else if (numericOnly) {
      const int idx = selectedCol + selectedRow * NUMERIC_COLS;
      if (idx < NUMERIC_KEY_COUNT) {
        const char selDigitBuf[2] = {numericKeys[idx], '\0'};
        GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, selDigitBuf, true, nullptr,
                            KeyboardKeyType::Normal, true);
      }
    } else if (arabicMode) {
      GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, arabicLayout[selectedRow][selectedCol],
                          true, nullptr, KeyboardKeyType::Normal, true);
    } else {
      const KeyDef& selKey = layout[selectedRow][selectedCol];
      char selPrimary = selKey.primary;
      char selSecondary = selKey.secondary;
      if (!symMode && shiftState > 0 && selKey.secondary != '\0') {
        selPrimary = selKey.secondary;
        selSecondary = selKey.primary;
      }
      const char selPrimaryBuf[2] = {selPrimary, '\0'};
      const char selSecondaryBuf[2] = {selSecondary, '\0'};
      const bool selShowSecondary = !symMode && selectedRow == 0 && selSecondary != '\0';
      GUI.drawKeyboardKey(renderer, Rect{selKeyX, selKeyY, selKeyW, selKeyH}, selPrimaryBuf, true,
                          selShowSecondary ? selSecondaryBuf : nullptr, KeyboardKeyType::Normal, true);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  GUI.drawSideButtonHints(renderer, ">", "<");

  renderer.displayBuffer();
}

void KeyboardEntryActivity::onComplete(std::string text) {
  setResult(KeyboardResult{std::move(text)});
  finish();
}

void KeyboardEntryActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
