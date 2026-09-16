#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/reader/DictionaryWordSelection.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Dictionary& dictionary,
                                        const char* query, std::string headword, std::string definition);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int x;
    int y;
    int width;
    int height;
  };

  BodyArea bodyArea() const;
  void prepareDefinition();
  void releaseDefinition();
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY) const;

  using Word = DictionaryWordSelection::Word;
  size_t collectWords(Word* output) const;
  void startSelection();
  void drawSelection(int fontId) const;
  void showMessage(StrId message);
  void showLookupError(Dictionary::LookupResult result);
  enum class Navigation { Forward, Back };
  void navigate(Navigation direction);

  static constexpr size_t HISTORY_CAPACITY = 8;
  static constexpr size_t QUERY_BYTES = 256;
  struct HistoryEntry {
    char query[QUERY_BYTES] = {};
    int page = 0;
  };
  HistoryEntry history[HISTORY_CAPACITY];
  size_t historySize = 0;
  char currentQuery[QUERY_BYTES] = {};
  char pendingQuery[QUERY_BYTES] = {};
  // Owned by the reader's word selector, which stays below us on the activity stack.
  Dictionary& dictionary;
  std::unique_ptr<Word[]> words;
  size_t wordCount = 0;
  int selected = 0;
  unsigned long lastHorizontalMoveTime = 0;
  bool showingMessage = false;
  StrId message = StrId::STR_DICT_NOT_FOUND;
  unsigned long messageTime = 0;

  std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text.
  std::string definition;
  const bool htmlDefinition;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;
};
