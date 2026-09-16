#include "DictionaryWordSelection.h"

#include <MappedInputManager.h>

#include <cctype>
#include <climits>
#include <cstdlib>

namespace DictionaryWordSelection {
namespace {

constexpr unsigned long REPEAT_START_MS = 500;
constexpr unsigned long REPEAT_INTERVAL_MS = 500;

}  // namespace

bool isSelectable(const char* text, const size_t length) {
  for (size_t i = 0; i < length; ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      if (std::isalnum(c)) return true;
    } else if (c == 0xE2 && i + 2 < length &&
               (static_cast<unsigned char>(text[i + 1]) == 0x80 || static_cast<unsigned char>(text[i + 1]) == 0x81)) {
      i += 2;
    } else {
      return true;
    }
  }
  return false;
}

int closestInRow(const Word* words, const size_t count, const uint16_t row, const int centerX) {
  int best = -1;
  int distance = INT_MAX;
  for (size_t i = 0; i < count; ++i) {
    if (words[i].row != row) continue;
    const int delta = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (delta < distance) {
      best = static_cast<int>(i);
      distance = delta;
    }
  }
  return best;
}

int wordAt(const Word* words, const size_t count, const int x, const int y, const int lineHeight) {
  constexpr int SLOP = 4;
  for (size_t i = 0; i < count; ++i) {
    const auto& word = words[i];
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP &&
        y < word.y + lineHeight + SLOP) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool move(MappedInputManager& input, const Word* words, const size_t count, int& selected,
          unsigned long& lastHorizontalMoveTime, const unsigned long now) {
  const bool repeat = input.getHeldTime() >= REPEAT_START_MS && now - lastHorizontalMoveTime >= REPEAT_INTERVAL_MS;
  const bool left = input.wasPressed(MappedInputManager::Button::ScreenLeft) ||
                    (repeat && input.isPressed(MappedInputManager::Button::ScreenLeft));
  const bool right = input.wasPressed(MappedInputManager::Button::ScreenRight) ||
                     (repeat && input.isPressed(MappedInputManager::Button::ScreenRight));
  if (left && selected > 0) {
    --selected;
    lastHorizontalMoveTime = now;
    return true;
  }
  if (right && selected + 1 < static_cast<int>(count)) {
    ++selected;
    lastHorizontalMoveTime = now;
    return true;
  }

  const int direction = input.wasPressed(MappedInputManager::Button::ScreenUp)     ? -1
                        : input.wasPressed(MappedInputManager::Button::ScreenDown) ? 1
                                                                                   : 0;
  if (direction == 0) return false;

  const auto& word = words[selected];
  int row = word.row + direction;
  int next = -1;
  while (row >= 0 && row <= words[count - 1].row && next < 0) {
    next = closestInRow(words, count, static_cast<uint16_t>(row), word.x + word.width / 2);
    row += direction;
  }
  if (next < 0) return false;
  selected = next;
  return true;
}

}  // namespace DictionaryWordSelection
