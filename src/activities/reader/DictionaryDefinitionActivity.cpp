#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;

// Body text left/right inset, matching the reader's default feel.
constexpr int SIDE_PADDING = 20;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

constexpr unsigned long MESSAGE_DURATION_MS = 1500;

bool isWordSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace

DictionaryDefinitionActivity::DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           Dictionary& dictionary, const char* query,
                                                           std::string headword, std::string definition)
    : Activity("DictionaryDefinition", renderer, mappedInput),
      dictionary(dictionary),
      headword(std::move(headword)),
      definition(std::move(definition)),
      htmlDefinition(dictionary.definitionsAreHtml()) {
  memcpy(currentQuery, query, strlen(query) + 1);
}

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  RenderLock lock;
  prepareDefinition();
  requestUpdate();
}

void DictionaryDefinitionActivity::prepareDefinition() {
  // Normalize StarDict multi-type separators so the wrap loop and the
  // C-string font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  if (!(htmlDefinition && definition.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages())) {
    definition = htmlToPlainText(definition);
    wrapText();
  }
}

void DictionaryDefinitionActivity::releaseDefinition() {
  words.reset();
  wordCount = 0;
  selected = 0;
  std::vector<std::unique_ptr<Page>>().swap(pages);
  std::vector<Line>().swap(lines);
  std::string().swap(definition);
  currentPage = 0;
  totalPages = 1;
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int topArea = (isInverted ? metrics.buttonHintsHeight : 0) + metrics.topPadding + metrics.headerHeight;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;
  return {(orientation == GfxRenderer::Orientation::LandscapeClockwise ? hintGutterWidth : 0) + SIDE_PADDING, topArea,
          renderer.getScreenWidth() - hintGutterWidth - 2 * SIDE_PADDING,
          renderer.getScreenHeight() - topArea - bottomArea};
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `definition` on success (the page arenas
// own the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  if (!buildDictionaryHtmlPages(renderer, definition, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), pages)) {
    return false;
  }
  definition.clear();
  definition.shrink_to_fit();
  totalPages = static_cast<int>(pages.size());
  currentPage = 0;
  return true;
}

int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
}

// Greedy word-wrap of `definition` into byte spans. '\n' breaks lines (blank
// lines survive as paragraph spacing; NULs from multi-type StarDict entries
// were normalized to newlines in onEnter); '\r' is dropped by treating it as
// a space at a token edge.
void DictionaryDefinitionActivity::wrapText() {
  lines.clear();
  lines.reserve(definition.size() / 32 + 8);

  const int fontId = SETTINGS.getReaderFontId();
  // SD-card fonts: merge every definition codepoint into the persistent
  // advance table up front. Otherwise each unseen codepoint measured below
  // falls back to an on-demand glyph load from SD (8-slot overflow ring).
  renderer.ensureSdCardFontReady(fontId, definition.c_str(), 0x01 /* REGULAR */);

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId);
  linesPerPage = std::max(1, body.height / lineHeight);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart)});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A
    // natural stop lands on whitespace or the terminating NUL, never on a
    // continuation byte, so this is a no-op there.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(fontId, text + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line
    // (any previous content was flushed above): split it at the widest
    // fitting UTF-8 boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(fontId, text + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      flushLine(rest);
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart) flushLine(n);

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  currentPage = 0;
}

size_t DictionaryDefinitionActivity::collectWords(Word* output) const {
  size_t count = 0;
  const int fontId = SETTINGS.getReaderFontId();
  const BodyArea body = bodyArea();
  const auto add = [&](const char* text, size_t length, int x, int y, int width, uint16_t row, uint8_t style) {
    if (!DictionaryWordSelection::isSelectable(text, length)) return;
    if (output) {
      output[count] = {text,
                       static_cast<int16_t>(x),
                       static_cast<int16_t>(y),
                       static_cast<int16_t>(width),
                       static_cast<uint16_t>(length),
                       row,
                       style};
    }
    ++count;
  };

  if (!pages.empty()) {
    uint16_t row = 0;
    for (const auto& element : pages[currentPage]->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto* line = static_cast<const PageLine*>(element.get());
      const auto* block = line->getBlock();
      if (!block || !block->valid()) continue;
      const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(fontId));
      const size_t before = count;
      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        const char* text = block->wordText(i);
        const auto style = block->wordStyle(i);
        add(text, block->wordTextLen(i), body.x + line->xPos + block->wordXpos(i), body.y + line->yPos + rubyShift,
            output ? renderer.getTextAdvanceX(fontId, text, style) : 0, row, static_cast<uint8_t>(style));
      }
      if (count != before) ++row;
    }
    return count;
  }

  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; ++i) {
    const char* text = definition.c_str() + lines[i].start;
    const size_t length = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    size_t pos = 0;
    while (pos < length) {
      while (pos < length && isWordSpace(text[pos])) ++pos;
      const size_t start = pos;
      while (pos < length && !isWordSpace(text[pos])) ++pos;
      if (pos == start) continue;
      add(text + start, pos - start, body.x + (output ? measureSpan(fontId, text, start) : 0),
          body.y + (i - firstLine) * renderer.getLineHeight(fontId),
          output ? measureSpan(fontId, text + start, pos - start) : 0, static_cast<uint16_t>(i - firstLine), 0);
    }
  }
  return count;
}

void DictionaryDefinitionActivity::startSelection() {
  wordCount = collectWords(nullptr);
  if (wordCount == 0) return;
  // Geometry is needed only while selecting, and is too large for the task stack.
  // Borrow text from the current layout instead of copying each word.
  words = makeUniqueNoThrow<Word[]>(wordCount);
  if (!words) {
    LOG_ERR("DICT", "OOM: definition word selection (%u words)", static_cast<unsigned>(wordCount));
    wordCount = 0;
    showMessage(StrId::STR_DICT_LOW_MEMORY);
    return;
  }
  collectWords(words.get());
  selected = 0;
  const int middle = DictionaryWordSelection::closestInRow(words.get(), wordCount, words[wordCount / 2].row,
                                                           renderer.getScreenWidth() / 2);
  if (middle >= 0) selected = middle;
  requestUpdate();
}

void DictionaryDefinitionActivity::drawSelection(const int fontId) const {
  if (!words) return;
  const auto& word = words[selected];
  // Outline the rendered word, preserving shaping, ruby and style runs underneath.
  renderer.drawRect(word.x - 2, word.y - 2, word.width + 4, renderer.getLineHeight(fontId) + 4);
}

void DictionaryDefinitionActivity::showMessage(const StrId value) {
  message = value;
  showingMessage = true;
  messageTime = millis();
  requestUpdate();
}

void DictionaryDefinitionActivity::showLookupError(const Dictionary::LookupResult result) {
  switch (result) {
    case Dictionary::LookupResult::LowMemory:
      showMessage(StrId::STR_DICT_LOW_MEMORY);
      break;
    case Dictionary::LookupResult::Decompress:
      showMessage(StrId::STR_DICT_DECOMPRESS_ERROR);
      break;
    case Dictionary::LookupResult::NotFound:
      showMessage(StrId::STR_DICT_NOT_FOUND);
      break;
    default:
      showMessage(StrId::STR_DICT_READ_FAILED);
      break;
  }
}

void DictionaryDefinitionActivity::navigate(const Navigation direction) {
  {
    RenderLock lock;
    if (direction == Navigation::Forward && historySize == HISTORY_CAPACITY) {
      showMessage(StrId::STR_DICT_HISTORY_FULL);
      return;
    }
    const char* query = currentQuery;
    size_t length = strlen(query);
    if (direction == Navigation::Forward) {
      query = words[selected].text;
      length = words[selected].length;
      if (pages.empty()) {
        // A long plain-text word may wrap across lines; look up the whole token.
        const char* end = query + length;
        while (query > definition.c_str() && !isWordSpace(query[-1])) --query;
        while (*end && !isWordSpace(*end)) ++end;
        length = end - query;
      }
    } else if (direction == Navigation::Back) {
      query = history[historySize - 1].query;
      length = strlen(query);
    }
    memcpy(pendingQuery, query, length);
    pendingQuery[length] = '\0';
    showMessage(StrId::STR_DICT_LOOKING_UP);
  }
  requestUpdateAndWait();

  std::string nextHeadword;
  Dictionary::LookupResult result;
  const auto location = dictionary.findEntry(pendingQuery, nextHeadword, &result);
  RenderLock lock;
  if (!location.found) {
    showLookupError(result);
    return;
  }

  const int oldPage = currentPage;
  const int targetPage = direction == Navigation::Back ? history[historySize - 1].page : oldPage;
  releaseDefinition();
  if (!dictionary.readDefinition(location, definition, &result)) {
    LOG_ERR("DICT", "Replacement failed (%d); restoring previous definition", static_cast<int>(result));
    releaseDefinition();  // release any capacity retained by a failed read
    Dictionary::LookupResult recoveryResult;
    std::string recoveredHeadword;
    if (dictionary.lookup(currentQuery, definition, recoveredHeadword, &recoveryResult)) {
      headword = std::move(recoveredHeadword);
      prepareDefinition();
      currentPage = std::min(oldPage, totalPages - 1);
    } else {
      LOG_ERR("DICT", "Definition recovery failed (%d)", static_cast<int>(recoveryResult));
      finish();
      return;
    }
    showLookupError(result);
    return;
  }

  if (direction == Navigation::Forward) {
    auto& entry = history[historySize++];
    memcpy(entry.query, currentQuery, strlen(currentQuery) + 1);
    entry.page = oldPage;
  } else if (direction == Navigation::Back) {
    --historySize;
  }
  memcpy(currentQuery, pendingQuery, strlen(pendingQuery) + 1);
  headword = std::move(nextHeadword);
  prepareDefinition();
  if (direction != Navigation::Forward) currentPage = std::min(targetPage, totalPages - 1);
  showingMessage = false;
  requestUpdate();
}

void DictionaryDefinitionActivity::loop() {
  RenderLock lock;
  if (showingMessage) {
    if (millis() - messageTime < MESSAGE_DURATION_MS) return;
    showingMessage = false;
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (words) {
      words.reset();
      wordCount = 0;
      requestUpdate();
    } else if (historySize != 0) {
      lock.unlock();
      navigate(Navigation::Back);
    } else {
      finish();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (words) {
      lock.unlock();
      navigate(Navigation::Forward);
    } else {
      startSelection();
    }
    return;
  }

  if (words) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTouchDown(tx, ty)) {
      const int hit = DictionaryWordSelection::wordAt(words.get(), wordCount, tx, ty,
                                                      renderer.getLineHeight(SETTINGS.getReaderFontId()));
      if (hit >= 0) {
        selected = hit;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const int hit = DictionaryWordSelection::wordAt(words.get(), wordCount, tx, ty,
                                                      renderer.getLineHeight(SETTINGS.getReaderFontId()));
      if (hit >= 0) {
        selected = hit;
        lock.unlock();
        navigate(Navigation::Forward);
      }
      return;
    }
    if (DictionaryWordSelection::move(mappedInput, words.get(), wordCount, selected, lastHorizontalMoveTime, millis()))
      requestUpdate();
    return;
  }

  // Same tap zones as the reader page turns: left third = previous page,
  // the rest = next. Back is the usual left-edge swipe.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (tx < renderer.getScreenWidth() / 3) {
      if (currentPage > 0) {
        currentPage--;
        requestUpdate();
      }
    } else if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
  });
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, definition.c_str() + lines[i].start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, x, startY + (i - firstLine) * lineHeight, buf);
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;

  // Header: matched headword left, page counter right.
  const int headerY = contentY + metrics.topPadding + 10;
  renderer.drawText(UI_12_FONT_ID, contentX + SIDE_PADDING, headerY, headword.c_str(), true, EpdFontFamily::BOLD);
  if (totalPages > 1) {
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, totalPages);
    const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - SIDE_PADDING - counterWidth, headerY, counter);
  }

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = SETTINGS.getReaderFontId();
  const int bodyStartY = contentY + metrics.topPadding + metrics.headerHeight;
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, contentX + SIDE_PADDING, bodyStartY);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, contentX + SIDE_PADDING, bodyStartY);
  drawSelection(fontId);

  if (words) {
    const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT),
                                                         tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOKUP), (currentPage > 0 ? "<" : ""),
                                              (currentPage + 1 < totalPages ? ">" : ""));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  if (showingMessage) {
    GUI.drawPopup(renderer, I18N.get(message));
    return;
  }
  renderer.displayBuffer();
}
