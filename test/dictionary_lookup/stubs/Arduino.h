#pragma once

#include <cstdint>

inline unsigned long millis() { return 0; }

struct DictionaryTestEsp {
  uint32_t largestBlock = 200 * 1024;
  uint32_t getMaxAllocHeap() const { return largestBlock; }
};
inline DictionaryTestEsp ESP;
