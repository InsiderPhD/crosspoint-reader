#pragma once
#include <GfxRenderer.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "../Activity.h"
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
                                 const size_t maxLength = 0, InputType inputType = InputType::Text)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        inputType(inputType) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The keyboard hit-tests taps itself; a tap on a key must not also inject
  // Confirm (X4 Pro tap-anywhere-is-Confirm).
  bool consumesTouchInput() const override { return true; }

 private:
  std::string title;
  std::string text;
  size_t maxLength;
  InputType inputType;
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

  // Geometry of the drawn key grid. Factored out of render() so touch
  // hit-testing resolves against exactly the rectangles that were drawn rather
  // than a second copy of the layout arithmetic.
  struct KeyGrid {
    int startY;
    int keyWidth;
    int keyHeight;
    int keySpacing;
    int contentLeftMargin;
    int contentRows;
    int contentCols;
    int bottomRowY;
    int bottomLeftMargin;
    int bottomKeyWidth;
    int bottomKeyHeight;
    int bottomKeySpacing;
  };
  KeyGrid computeKeyGrid(int keyboardStartY) const;

#if FREEINK_DEVICE_X4PRO
  // Top of the key grid as last drawn. Cached rather than recomputed because in
  // a non bottom-aligned theme it depends on the wrapped input field height,
  // which only render() measures. One aligned int: written by the render task,
  // read by the input task, so it cannot tear.
  int renderedKeyboardStartY = 0;

  // Logical-frame tap point -> key cell. False when the point misses the grid.
  bool hitTestKey(int lx, int ly, int& row, int& col) const;

  // Dispatches screen taps and long presses onto keys. Returns false when the
  // activity finished (Ok pressed) and loop() must not touch state again.
  bool handleTouchInput();
#endif

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
