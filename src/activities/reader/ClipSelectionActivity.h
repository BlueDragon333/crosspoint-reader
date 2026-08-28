#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ClipSelectionActivity final : public Activity {
 public:
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                        int marginLeft, int marginTop) = delete;
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        std::vector<std::unique_ptr<Page>> pages, int marginLeft, int marginTop);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleHomeGesture() override;

 private:
  struct WordBox {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
    uint16_t row = 0;
    uint8_t pageOffset = 0;
    uint16_t pageWordIndex = 0;
    const char* text = nullptr;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool paragraphStart = false;
  };

  static constexpr size_t MAX_SELECTABLE_WORDS = 240;

  void extractWords();
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  void selectIndex(int index);
  void moveToPage(int pageOffset);
  void confirmSelection();
  void cancel();
  std::string buildSelectedText(int first, int last) const;
  void drawSelection() const;

  std::vector<std::unique_ptr<Page>> pages;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  int lineHeight = 0;
  std::vector<WordBox> words;
  int selected = 0;
  int rangeStart = -1;
  uint8_t currentPageOffset = 0;
  uint16_t rowCount = 0;
  ButtonNavigator buttonNavigator;
};
