#include "ClipSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>

#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"

namespace {

constexpr size_t FONT_PREWARM_TEXT_MAX = 2048;

bool hasVisibleText(const char* text) {
  if (!text) return false;
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p != 0; ++p) {
    if (*p > ' ') return true;
  }
  return false;
}

bool hasEmSpacePrefix(const char* text) {
  return text && static_cast<uint8_t>(text[0]) == 0xE2 && static_cast<uint8_t>(text[1]) == 0x80 &&
         static_cast<uint8_t>(text[2]) == 0x83;
}

std::string cleanWord(const char* text) {
  if (!text) return {};
  if (hasEmSpacePrefix(text)) text += 3;
  std::string result;
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p != 0;) {
    if (*p == '\r' || *p == '\n' || *p == '\t') {
      if (!result.empty() && result.back() != ' ') result.push_back(' ');
      ++p;
      continue;
    }
    if (*p == 0xC2 && p[1] == 0xA0) {
      if (!result.empty() && result.back() != ' ') result.push_back(' ');
      p += 2;
      continue;
    }
    result.push_back(static_cast<char>(*p++));
  }
  while (!result.empty() && result.front() == ' ') result.erase(result.begin());
  while (!result.empty() && result.back() == ' ') result.pop_back();
  return result;
}

}  // namespace

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<std::unique_ptr<Page>> pages, const int marginLeft,
                                             const int marginTop)
    : Activity("ClipSelection", renderer, mappedInput),
      pages(std::move(pages)),
      marginLeft(marginLeft),
      marginTop(marginTop) {}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  if (!extractWords() || wordCount == 0) {
    if (wordCount == 0) LOG_ERR("CLIP", "No selectable words on current page");
    cancel();
    return;
  }
  uint16_t firstPageRows = 0;
  for (size_t i = 0; i < wordCount; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != 0) break;
    firstPageRows = std::max<uint16_t>(firstPageRows, static_cast<uint16_t>(word.row + 1));
  }
  const int middle = closestInRow(firstPageRows / 2, renderer.getScreenWidth() / 2);
  if (middle >= 0) selected = middle;
  requestUpdate();
}

bool ClipSelectionActivity::extractWords() {
  wordCount = 0;
  words = makeUniqueNoThrow<WordBox[]>(MAX_SELECTABLE_WORDS);
  if (!words) {
    LOG_ERR("CLIP", "OOM: selection words (%u bytes)", static_cast<unsigned>(MAX_SELECTABLE_WORDS * sizeof(WordBox)));
    return false;
  }
  rowCount = 0;
  uint16_t pageWordIndex = 0;
  const bool needsFontPrewarm = renderer.isSdCardFont(fontId);
  auto pageText = needsFontPrewarm ? makeUniqueNoThrow<char[]>(FONT_PREWARM_TEXT_MAX) : nullptr;
  size_t pageTextLength = 0;
  if (needsFontPrewarm && !pageText) LOG_DBG("CLIP", "Skipping SD font prewarm: OOM");
  uint8_t styleMask = 0;

  for (size_t pageOffset = 0; pageOffset < pages.size(); ++pageOffset) {
    pageWordIndex = 0;
    for (const auto& element : pages[pageOffset]->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = line.getBlock();
      if (!block || !block->valid()) continue;

      const size_t lineStart = wordCount;
      const bool isRtl = block->getBlockStyle().isRtl;
      const size_t remaining = MAX_SELECTABLE_WORDS - lineStart;
      size_t rtlWordCount = 0;
      const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(fontId));
      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        const char* text = block->wordText(i);
        if (!hasVisibleText(text)) continue;

        const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
        int width = renderer.getTextAdvanceX(fontId, text, style);
        if (width <= 0) continue;
        if (i + 1 < block->wordCount() && block->wordXpos(i + 1) > block->wordXpos(i)) {
          width = std::min(width, static_cast<int>(block->wordXpos(i + 1) - block->wordXpos(i)));
        }

        if (!isRtl && wordCount == MAX_SELECTABLE_WORDS) break;

        WordBox& word = isRtl ? words[lineStart + (rtlWordCount < remaining ? rtlWordCount : rtlWordCount % remaining)]
                              : words[wordCount++];
        word.x = static_cast<int16_t>(marginLeft + line.xPos + block->wordXpos(i));
        word.y = static_cast<int16_t>(marginTop + line.yPos + rubyShift);
        word.width = static_cast<int16_t>(width);
        word.height = static_cast<int16_t>(lineHeight);
        word.row = rowCount;
        word.pageOffset = static_cast<uint8_t>(pageOffset);
        word.pageWordIndex = pageWordIndex++;
        word.text = text;
        word.style = style;
        word.paragraphStart = hasEmSpacePrefix(text);
        if (pageText) {
          for (const char* p = text; *p != '\0' && pageTextLength + 1 < FONT_PREWARM_TEXT_MAX; ++p) {
            pageText[pageTextLength++] = *p;
          }
          if (pageTextLength + 1 < FONT_PREWARM_TEXT_MAX) pageText[pageTextLength++] = ' ';
        }
        styleMask |= static_cast<uint8_t>(1U << (static_cast<uint8_t>(style) & 0x03));
        if (isRtl) ++rtlWordCount;
      }
      if (isRtl) {
        const size_t stored = std::min(remaining, rtlWordCount);
        wordCount = lineStart + stored;
        if (rtlWordCount > remaining) {
          std::rotate(words.get() + lineStart, words.get() + lineStart + rtlWordCount % remaining,
                      words.get() + wordCount);
        }
        std::reverse(words.get() + lineStart, words.get() + wordCount);
      }
      if (wordCount != lineStart) ++rowCount;
      if (wordCount == MAX_SELECTABLE_WORDS) {
        LOG_ERR("CLIP", "Selectable word cap hit (%u); multi-page selection was truncated",
                static_cast<unsigned>(MAX_SELECTABLE_WORDS));
        break;
      }
    }
    if (wordCount == MAX_SELECTABLE_WORDS) break;
  }

  if (styleMask == 0) styleMask = 0x01;
  if (pageText) {
    pageText[pageTextLength] = '\0';
    renderer.ensureSdCardFontReady(fontId, pageText.get(), styleMask);
  }

  const int indentThreshold = lineHeight / 2;
  int previousRowFirst = -1;
  for (size_t i = 0; i < wordCount; ++i) {
    if (i > 0 && words[i].row == words[i - 1].row) continue;
    if (previousRowFirst >= 0 && words[i].pageOffset == words[previousRowFirst].pageOffset &&
        words[i].x > words[previousRowFirst].x + indentThreshold) {
      words[i].paragraphStart = true;
    }
    previousRowFirst = static_cast<int>(i);
  }
  return true;
}

int ClipSelectionActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

int ClipSelectionActivity::wordAt(const int x, const int y) const {
  constexpr int SLOP = 4;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP && y < word.y + word.height + SLOP) {
      return i;
    }
  }
  return -1;
}

void ClipSelectionActivity::moveVertical(const int direction) {
  const int targetRow = static_cast<int>(words[selected].row) + direction;
  if (targetRow < 0 || targetRow >= rowCount) return;
  const int next = closestInRow(static_cast<uint16_t>(targetRow), words[selected].x + words[selected].width / 2);
  if (next >= 0 && next != selected) {
    selectIndex(next);
  }
}

void ClipSelectionActivity::selectIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(wordCount) || index == selected) return;
  selected = index;
  currentPageOffset = words[selected].pageOffset;
  requestUpdate();
}

void ClipSelectionActivity::moveToPage(const int pageOffset) {
  if (pageOffset < 0 || pageOffset >= static_cast<int>(pages.size()) || pageOffset == currentPageOffset) return;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].pageOffset == pageOffset) {
      selectIndex(i);
      return;
    }
  }
}

std::string ClipSelectionActivity::buildSelectedText(const int first, const int last) const {
  std::string text;
  text.reserve(256);
  for (int i = first; i <= last; ++i) {
    std::string word = cleanWord(words[i].text);
    if (word.empty()) continue;
    if (!text.empty()) {
      const WordBox& previous = words[i - 1];
      if (!text.empty() && text.back() == '-' && word.front() != '-' &&
          std::isalnum(static_cast<unsigned char>(word.front()))) {
        text.pop_back();
      } else if (words[i].paragraphStart) {
        text.push_back('\n');
      } else {
        const bool visuallyAttached =
            words[i].row == previous.row && std::abs(words[i].x - (previous.x + previous.width)) <= 2;
        if (!visuallyAttached) text.push_back(' ');
      }
    }
    if (text.size() >= CLIPPING_TEXT_MAX) break;
    text.append(word, 0, CLIPPING_TEXT_MAX - text.size());
  }
  return text;
}

void ClipSelectionActivity::confirmSelection() {
  if (rangeStart < 0) {
    rangeStart = selected;
    requestUpdate();
    return;
  }

  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  ClippingResult result;
  result.text = buildSelectedText(first, last);
  result.startPageOffset = words[first].pageOffset;
  result.endPageOffset = words[last].pageOffset;
  result.startWordIndex = words[first].pageWordIndex;
  result.endWordIndex = words[last].pageWordIndex;
  result.wordCount = static_cast<uint16_t>(last - first + 1);
  setResult(std::move(result));
  finish();
}

void ClipSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

bool ClipSelectionActivity::handleHomeGesture() {
  cancel();
  return true;
}

void ClipSelectionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (rangeStart >= 0) {
      rangeStart = -1;
      requestUpdate();
    } else {
      cancel();
    }
    return;
  }
  if (wordCount == 0) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirmSelection();
    return;
  }

  int touchX = 0;
  int touchY = 0;
  if (mappedInput.wasScreenTapped(touchX, touchY)) {
    const int hit = wordAt(touchX, touchY);
    if (hit >= 0) {
      selectIndex(hit);
      confirmSelection();
    }
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
    moveToPage(static_cast<int>(currentPageOffset) + 1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
    moveToPage(static_cast<int>(currentPageOffset) - 1);
    return;
  }

  buttonNavigator.onPrevious([this] {
    if (selected > 0) {
      selectIndex(selected - 1);
    }
  });
  buttonNavigator.onNext([this] {
    if (selected + 1 < static_cast<int>(wordCount)) {
      selectIndex(selected + 1);
    }
  });
  if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) moveVertical(-1);
  if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) moveVertical(1);
}

void ClipSelectionActivity::drawSelection() const {
  const int first = rangeStart < 0 ? selected : std::min(rangeStart, selected);
  const int last = rangeStart < 0 ? selected : std::max(rangeStart, selected);
  for (int i = first; i <= last; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    renderer.fillRectDither(word.x, word.y, word.width, word.height, Color::LightGray);
    renderer.drawText(fontId, word.x, word.y, word.text, true, word.style);
  }
  const WordBox& cursor = words[selected];
  renderer.drawRect(cursor.x, cursor.y, cursor.width, cursor.height, true);
}

void ClipSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  pages[currentPageOffset]->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  pages[currentPageOffset]->render(renderer, fontId, marginLeft, marginTop);
  if (wordCount != 0) drawSelection();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), rangeStart < 0 ? tr(STR_SELECT) : tr(STR_DONE),
                                            tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
