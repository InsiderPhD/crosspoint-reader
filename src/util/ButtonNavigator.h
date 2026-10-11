#pragma once

#include <functional>
#include <initializer_list>
#include <vector>  // not used here; kept because includers have relied on it transitively

#include "MappedInputManager.h"

class ButtonNavigator final {
  using Callback = std::function<void()>;
  // A borrowed view over a constant array: consumed synchronously inside the
  // on*() call and never stored. This was std::vector, so every poll of every
  // list screen heap-allocated (and freed) a two-element vector per on*() call,
  // i.e. several small malloc/free pairs per loop pass for the whole time a
  // menu was open. A braced list of enum constants is backed by a static
  // read-only array, so call sites keep writing {Button::Left}.
  using Buttons = std::initializer_list<MappedInputManager::Button>;

  const uint16_t continuousStartMs;
  const uint16_t continuousIntervalMs;
  uint32_t lastContinuousNavTime = 0;
  static const MappedInputManager* mappedInput;

  [[nodiscard]] bool shouldNavigateContinuously(MappedInputManager::Button button) const;

 public:
  explicit ButtonNavigator(const uint16_t continuousIntervalMs = 500, const uint16_t continuousStartMs = 500)
      : continuousStartMs(continuousStartMs), continuousIntervalMs(continuousIntervalMs) {}

  static void setMappedInputManager(const MappedInputManager& mappedInputManager) { mappedInput = &mappedInputManager; }

  void onNext(const Callback& callback);
  void onPrevious(const Callback& callback);
  void onPressAndContinuous(const Buttons& buttons, const Callback& callback);

  void onNextPress(const Callback& callback);
  void onPreviousPress(const Callback& callback);
  void onPress(const Buttons& buttons, const Callback& callback);

  void onNextRelease(const Callback& callback);
  void onPreviousRelease(const Callback& callback);
  void onRelease(const Buttons& buttons, const Callback& callback);

  void onNextContinuous(const Callback& callback);
  void onPreviousContinuous(const Callback& callback);
  void onContinuous(const Buttons& buttons, const Callback& callback);

  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems);

  [[nodiscard]] static int nextPageIndex(int currentIndex, int totalItems, int itemsPerPage);
  [[nodiscard]] static int previousPageIndex(int currentIndex, int totalItems, int itemsPerPage);

  // Static storage: returning a braced list directly would hand back a view of
  // an array that dies with the function's return statement.
  [[nodiscard]] static Buttons getNextButtons() {
    static constexpr Buttons buttons = {MappedInputManager::Button::Down, MappedInputManager::Button::Right};
    return buttons;
  }
  [[nodiscard]] static Buttons getPreviousButtons() {
    static constexpr Buttons buttons = {MappedInputManager::Button::Up, MappedInputManager::Button::Left};
    return buttons;
  }
};