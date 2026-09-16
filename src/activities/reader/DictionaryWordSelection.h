#pragma once

#include <cstddef>
#include <cstdint>

class MappedInputManager;

namespace DictionaryWordSelection {

struct Word {
  const char* text;
  int16_t x;
  int16_t y;
  int16_t width;
  uint16_t length;
  uint16_t row;
  uint8_t style;
};

bool isSelectable(const char* text, size_t length);
int closestInRow(const Word* words, size_t count, uint16_t row, int centerX);
int wordAt(const Word* words, size_t count, int x, int y, int lineHeight);
bool move(MappedInputManager& input, const Word* words, size_t count, int& selected,
          unsigned long& lastHorizontalMoveTime, unsigned long now);

}  // namespace DictionaryWordSelection
