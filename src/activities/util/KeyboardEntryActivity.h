#pragma once
#include <GfxRenderer.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct KeyDef {
  char primary;
  char secondary;
};

enum class SpecialKeyType { Shift, Mode, Space, Del, Ok };

enum class InputType { Text, Password, Url };

class KeyboardEntryActivity : public Activity {
 public:
  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "",
                                 const size_t maxLength = 0, InputType inputType = InputType::Text,
                                 std::string customTip = "", bool numericOnly = false)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        inputType(inputType),
        customTip(std::move(customTip)),
        numericOnly(numericOnly) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string title;
  std::string text;
  size_t maxLength;
  InputType inputType;
  // When non-empty, shown as the sole "Tips:" line instead of the normal mode-dependent
  // hints -- lets a specific caller (e.g. Foulad eBooks login) give context-specific
  // guidance without hardcoding that context into this generic widget.
  std::string customTip;
  // Swaps the full QWERTY layout for a compact digit-only keypad (mirrors urlMode's flat
  // restricted grid). Orthogonal to InputType -- e.g. Foulad eBooks' password field is
  // both Password (masked) AND numericOnly (foulad-ebooks enforces numeric-only
  // credentials server-side), which a single enum value couldn't express cleanly.
  bool numericOnly = false;
  bool passwordVisible = false;

  ButtonNavigator buttonNavigator;

  int selectedRow = 0;
  int selectedCol = 0;
  int shiftState = 0;
  bool symMode = false;
  bool confirmHeld = false;
  bool confirmLongHandled = false;

  bool cursorMode = false;
  bool togglePos = false;
  size_t cursorPos = 0;
  bool upHeld = false;
  bool upLongHandled = false;
  bool downHeld = false;
  bool downLongHandled = false;
  bool rightHeld = false;
  bool rightLongHandled = false;
  size_t savedCursorPos = 0;
  size_t rightStartCursorPos = 0;

  bool urlMode = false;
  static constexpr int URL_SNIPPET_COUNT = 9;
  static constexpr const char* const urlSnippets[URL_SNIPPET_COUNT] = {
      "https://", "www.", ".com", "http://", "192.168.", ".org", "/opds", ":8080", ".net"};

  // Flat 3-column digit grid for InputType::Numeric, same indexing scheme as urlSnippets
  // (idx = col + row * NUMERIC_COLS): rows of 1-2-3 / 4-5-6 / 7-8-9 / 0.
  static constexpr int NUMERIC_COLS = 3;
  static constexpr int NUMERIC_KEY_COUNT = 10;
  static constexpr char numericKeys[NUMERIC_KEY_COUNT] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};

  int delPressCount = 0;
  bool hintVisible = false;
  unsigned long hintShowTime = 0;

  void onComplete(std::string text);
  void onCancel();

  static constexpr uint16_t LONG_PRESS_MS = 500;
  static constexpr uint16_t DEL_LONG_PRESS_MS = 1500;

  static constexpr int COLS = 10;
  static constexpr int ABC_ROWS = 4;
  static constexpr int SYM_ROWS = 4;
  static constexpr int BOTTOM_KEY_COUNT = 5;

  static constexpr KeyDef abcLayout[ABC_ROWS][COLS] = {
      {{'1', '!'},
       {'2', '@'},
       {'3', '#'},
       {'4', '$'},
       {'5', '%'},
       {'6', '^'},
       {'7', '&'},
       {'8', '*'},
       {'9', '('},
       {'0', ')'}},
      {{'q', 'Q'},
       {'w', 'W'},
       {'e', 'E'},
       {'r', 'R'},
       {'t', 'T'},
       {'y', 'Y'},
       {'u', 'U'},
       {'i', 'I'},
       {'o', 'O'},
       {'p', 'P'}},
      {{'a', 'A'},
       {'s', 'S'},
       {'d', 'D'},
       {'f', 'F'},
       {'g', 'G'},
       {'h', 'H'},
       {'j', 'J'},
       {'k', 'K'},
       {'l', 'L'},
       {'-', '_'}},
      {{'z', 'Z'},
       {'x', 'X'},
       {'c', 'C'},
       {'v', 'V'},
       {'b', 'B'},
       {'n', 'N'},
       {'m', 'M'},
       {'=', '+'},
       {'.', '>'},
       {',', '<'}},
  };

  static constexpr KeyDef urlLayout[ABC_ROWS][COLS] = {
      {{'1', '!'},
       {'2', '@'},
       {'3', '#'},
       {'4', '$'},
       {'5', '%'},
       {'6', '^'},
       {'7', '&'},
       {'8', '*'},
       {'9', '('},
       {'0', ')'}},
      {{'q', 'Q'},
       {'w', 'W'},
       {'e', 'E'},
       {'r', 'R'},
       {'t', 'T'},
       {'y', 'Y'},
       {'u', 'U'},
       {'i', 'I'},
       {'o', 'O'},
       {'p', 'P'}},
      {{'a', 'A'},
       {'s', 'S'},
       {'d', 'D'},
       {'f', 'F'},
       {'g', 'G'},
       {'h', 'H'},
       {'j', 'J'},
       {'k', 'K'},
       {'l', 'L'},
       {'-', '_'}},
      {{'z', 'Z'},
       {'x', 'X'},
       {'c', 'C'},
       {'v', 'V'},
       {'b', 'B'},
       {'n', 'N'},
       {'m', 'M'},
       {':', '+'},
       {'.', '>'},
       {'/', '<'}},
  };

  static constexpr KeyDef symLayout[SYM_ROWS][COLS] = {
      {{'1', '\0'},
       {'2', '\0'},
       {'3', '\0'},
       {'4', '\0'},
       {'5', '\0'},
       {'6', '\0'},
       {'7', '\0'},
       {'8', '\0'},
       {'9', '\0'},
       {'0', '\0'}},
      {{'!', '\0'},
       {'@', '\0'},
       {'#', '\0'},
       {'$', '\0'},
       {'%', '\0'},
       {'^', '\0'},
       {'&', '\0'},
       {'*', '\0'},
       {'(', '\0'},
       {')', '\0'}},
      {{'-', '\0'},
       {'_', '\0'},
       {'=', '\0'},
       {'+', '\0'},
       {'[', '\0'},
       {']', '\0'},
       {'{', '\0'},
       {'}', '\0'},
       {';', '\0'},
       {':', '\0'}},
      {{'\'', '\0'},
       {'"', '\0'},
       {'/', '\0'},
       {'\\', '\0'},
       {'|', '\0'},
       {'?', '\0'},
       {'.', '\0'},
       {',', '\0'},
       {'~', '\0'},
       {'`', '\0'}},
  };

  static const char* const shiftString[2];

  int getContentRowCount() const;
  int getContentColCount() const;
  int getTotalRowCount() const;
  bool isBottomRow(int row) const;
  char getSelectedChar() const;
  char getAlternativeChar() const;
  bool handleKeyPress();
  bool insertChar(char c);
  void insertString(const std::string& str);
  void mapColContentBottom(int& col, bool goingUp) const;
};
