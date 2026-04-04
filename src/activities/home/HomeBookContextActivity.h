#pragma once
#include <string>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class HomeBookContextActivity final : public Activity {
  std::string bookTitle;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;

  static constexpr int OPTION_COUNT = 2;
  static constexpr int OPTION_MARK_AS_READ = 0;
  static constexpr int OPTION_REMOVE = 1;
  bool awaitingConfirmRelease = false;  // Ignore first release if button held on entry

 public:
  explicit HomeBookContextActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const std::string& bookTitle)
      : Activity("HomeBookContext", renderer, mappedInput), bookTitle(bookTitle) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
