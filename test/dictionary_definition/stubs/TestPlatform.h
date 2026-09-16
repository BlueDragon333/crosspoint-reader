#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

inline unsigned long testTime = 0;
inline unsigned long millis() { return testTime; }
#define LOG_ERR(...) ((void)0)

enum class StrId {
  STR_DICT_NOT_FOUND,
  STR_DICT_LOW_MEMORY,
  STR_DICT_DECOMPRESS_ERROR,
  STR_DICT_READ_FAILED,
  STR_DICT_HISTORY_FULL,
  STR_DICT_LOOKING_UP
};
#define tr(key) #key
inline struct TestI18n {
  const char* get(StrId) const { return "message"; }
} I18N;

struct EpdFontFamily {
  enum Style { REGULAR, BOLD };
};
struct FontCacheManager {
  struct Scope {
    void endScanAndPrewarm() {}
  };
  Scope createPrewarmScope() { return {}; }
  void releaseSdFontCaches() {}
};
struct GfxRenderer {
  enum class Orientation { Portrait, PortraitInverted, LandscapeClockwise, LandscapeCounterClockwise };
  Orientation orientation = Orientation::Portrait;
  FontCacheManager fonts;
  Orientation getOrientation() const { return orientation; }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int) const { return 20; }
  int getFontAscenderSize(int) const { return 15; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 8; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return static_cast<int>(strlen(text)) * 8;
  }
  int getTextWidth(int font, const char* text) const { return getTextAdvanceX(font, text); }
  void ensureSdCardFontReady(int, const char*, int) {}
  FontCacheManager* getFontCacheManager() { return &fonts; }
  void clearScreen() {}
  void drawText(int, int, int, const char*, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR) {}
  void drawRect(int, int, int, int) {}
  void displayBuffer() {}
};
inline struct TestSettings {
  int getReaderFontId() const { return 1; }
} SETTINGS;
inline constexpr int UI_12_FONT_ID = 2;
inline constexpr int UI_10_FONT_ID = 3;

class RenderLock {
  bool locked = true;

 public:
  static inline int depth = 0;
  RenderLock() { ++depth; }
  ~RenderLock() { unlock(); }
  void unlock() {
    if (locked) {
      --depth;
      locked = false;
    }
  }
};
struct MappedInputManager {
  enum class Button { None, Back, Confirm, ScreenLeft, ScreenRight, ScreenUp, ScreenDown };
  Button released = Button::None;
  Button pressed = Button::None;
  bool wasReleased(Button button) const { return button == released; }
  bool wasPressed(Button button) const { return button == pressed; }
  bool isPressed(Button) const { return false; }
  unsigned long getHeldTime() const { return 0; }
  bool wasScreenTapped(int&, int&) const { return false; }
  bool wasScreenTouchDown(int&, int&) const { return false; }
  struct Labels {
    const char *btn1, *btn2, *btn3, *btn4;
  };
  Labels mapLabels(const char* a, const char* b, const char* c, const char* d) const { return {a, b, c, d}; }
  Labels mapDirectionalLabels(const char* a, const char* b, const char* c, const char* d, const char*,
                              const char*) const {
    return {a, b, c, d};
  }
};
class Activity {
 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

 public:
  static inline bool finished = false;
  Activity(const char*, GfxRenderer& renderer, MappedInputManager& input) : renderer(renderer), mappedInput(input) {}
  virtual ~Activity() = default;
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual void loop() {}
  virtual void render(RenderLock&&) {}
  void requestUpdate() {}
  void requestUpdateAndWait() {
    assert(RenderLock::depth == 0);
    render(RenderLock{});
  }
  static void finish() { finished = true; }
};
struct ButtonNavigator {
  template <class F>
  void onNext(F) {}
  template <class F>
  void onPrevious(F) {}
};
struct UITheme {
  struct Metrics {
    int sideButtonHintsWidth = 30, buttonHintsHeight = 30, topPadding = 10, headerHeight = 40, verticalSpacing = 10;
  } metrics;
  static UITheme& getInstance() {
    static UITheme theme;
    return theme;
  }
  const Metrics& getMetrics() const { return metrics; }
  void drawButtonHints(GfxRenderer&, const char*, const char*, const char*, const char*) {}
  void drawPopup(GfxRenderer&, const char*) {}
};
#define GUI UITheme::getInstance()

struct TextBlock {
  std::string text;
  bool valid() const { return true; }
  int getRubyShift(int) const { return 0; }
  uint16_t wordCount() const { return 1; }
  const char* wordText(uint16_t) const { return text.c_str(); }
  uint16_t wordTextLen(uint16_t) const { return static_cast<uint16_t>(text.size()); }
  EpdFontFamily::Style wordStyle(uint16_t) const { return EpdFontFamily::REGULAR; }
  int wordXpos(uint16_t) const { return 0; }
};
inline constexpr int TAG_PageLine = 1;
struct PageLine {
  TextBlock block;
  int xPos = 0, yPos = 0;
  int getTag() const { return TAG_PageLine; }
  const TextBlock* getBlock() const { return &block; }
};
struct Page {
  std::vector<std::unique_ptr<PageLine>> elements;
  void render(GfxRenderer&, int, int, int) const {}
};

struct DictLocation {
  uint32_t offset = 0, size = 0;
  bool found = false, readError = false;
};
struct Dictionary {
  enum class LookupResult { Found, NotFound, LowMemory, Decompress, ReadError };
  struct Entry {
    std::string definition;
    int failures = 0;
  };
  std::map<std::string, Entry> entries;
  std::vector<std::string> searches, reads;
  std::string pending;
  std::function<void()> beforeRead;
  bool html = false;
  bool definitionsAreHtml() const { return html; }
  DictLocation findEntry(const char* query, std::string& headword, LookupResult* result) {
    searches.emplace_back(query);
    pending = query;
    const bool found = entries.count(query);
    *result = found ? LookupResult::Found : LookupResult::NotFound;
    if (found) headword = query;
    return {0, 0, found, false};
  }
  bool readDefinition(const DictLocation&, std::string& out, LookupResult* result) {
    if (beforeRead) beforeRead();
    reads.push_back(pending);
    auto& entry = entries.at(pending);
    if (entry.failures > 0) {
      --entry.failures;
      *result = LookupResult::ReadError;
      return false;
    }
    out = entry.definition;
    *result = LookupResult::Found;
    return true;
  }
  bool lookup(const char* query, std::string& out, std::string& headword, LookupResult* result) {
    const auto location = findEntry(query, headword, result);
    return location.found && readDefinition(location, out, result);
  }
};

inline bool buildDictionaryHtmlPages(GfxRenderer&, const std::string& definition, uint16_t, uint16_t,
                                     std::vector<std::unique_ptr<Page>>& pages) {
  auto page = std::make_unique<Page>();
  auto line = std::make_unique<PageLine>();
  line->block.text = definition;
  page->elements.push_back(std::move(line));
  pages.push_back(std::move(page));
  return true;
}
