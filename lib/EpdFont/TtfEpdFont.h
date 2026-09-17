#pragma once

// CrossPoint <- FreeInkFont adapter.
//
// Renders TrueType/OpenType through the FreeInkFont engine (freeink::font, an
// stb_truetype backend) and packs the results into CrossPoint's own EpdFontData
// glyph tables. The existing GfxRenderer / EpdFont draw path then renders TTF
// text with NO changes to layout or rendering — a TTF-backed font simply looks
// like any other EpdFont.
//
// Glyph cache model: incremental + bounded. Glyphs are rasterized once, on
// demand, and appended into a FIXED, pre-allocated arena (so the EpdFontData
// pointers the renderer holds never move). addCoverage() adds only the glyphs
// it hasn't seen yet — it never re-rasterizes resident glyphs. When the arena
// or glyph table fills, the whole cache flushes and rebuilds from the current
// request, so memory is bounded by the arena size regardless of how long a
// session runs. Size the arena for one page's worth of unique glyphs.
//
// Usage:
//   static uint8_t g_ttf[...];               // font file bytes, kept resident
//   TtfEpdFont ui;
//   ui.load(g_ttf, g_ttf_len, /*sizePx=*/28);
//   renderer.insertFont(FONT_ID, ui.family());
//   renderer.registerTtfFont(FONT_ID, &ui);  // rebuilds per page via addCoverage
//
// Lifetime: the TTF bytes are BORROWED (point at PSRAM / mmap / a resident
// heap buffer) and this object must outlive any GfxRenderer registration.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <FontArena.h>  // freeink::font::Arena  (FreeInkFont)
#include <TtfFont.h>    // freeink::font::TtfFont

#include "EpdFont.h"
#include "EpdFontData.h"
#include "EpdFontFamily.h"

class TtfEpdFont {
 public:
  // Borrow `ttfData` (must outlive this object) and prepare rasterization at
  // `sizePx`. `rasterCacheBytes` backs the FreeInkFont rasterizer; `glyphCacheBytes`
  // and `maxGlyphs` bound the packed EpdFont glyph cache (one page's worth).
  bool load(const uint8_t* ttfData, uint32_t ttfLen, uint16_t sizePx, size_t rasterCacheBytes = 48 * 1024,
            size_t glyphCacheBytes = 96 * 1024, uint16_t maxGlyphs = 1536);

  // Replace the resident glyph set with exactly the codepoints in `utf8`
  // (plus the replacement glyph). `twoBit` = 4-level antialiased gray.
  bool build(const char* utf8, bool twoBit = true);
  bool build(const std::deque<std::string>& words, bool includeHyphen, bool twoBit = true);

  // Incrementally ADD codepoints without evicting resident glyphs (only new
  // ones are rasterized). This is what the renderer's per-text hook uses: layout
  // visits every paragraph before the page is drawn, so accumulating guarantees
  // each drawn glyph is resident. Flushes + rebuilds only if the arena fills.
  bool addCoverage(const char* utf8, bool twoBit = true);
  bool addCoverage(const std::deque<std::string>& words, bool includeHyphen, bool twoBit = true);

  bool ready() const { return ready_; }
  uint16_t sizePx() const { return sizePx_; }

  // Renderer-facing handles. Valid while this object is alive; the EpdFont
  // pointer itself is stable across rebuilds (only the glyph data it points to
  // changes, and only between — never during — a render pass).
  const EpdFont* epdFont() const { return loaded_ ? &font_ : nullptr; }
  EpdFontFamily family() const { return EpdFontFamily(epdFont()); }

 private:
  // Ensure `cp` is resident: no-op if present, else rasterize + insert. Returns
  // false only when the cache is full (caller flushes and retries).
  bool ensureGlyph(uint32_t cp);
  void flushGlyphs();
  void refreshData();  // point data_ at the current buffers + metrics
  // Add `cp`s (already font-covered) with flush-on-overflow, then refreshData().
  bool addCps(const std::vector<uint32_t>& want, bool replace);

  freeink::font::TtfFont ttf_;
  std::vector<uint8_t> rasterBuf_;  // FreeInkFont rasterizer cache
  freeink::font::Arena rasterArena_;
  uint16_t sizePx_ = 0;
  bool loaded_ = false;
  bool ready_ = false;
  bool twoBit_ = true;

  // Bounded, incremental packed-glyph cache. Fixed capacity so the pointers in
  // data_ never move. cps_ and glyphs_ stay sorted-parallel by codepoint.
  std::vector<uint8_t> bmpArena_;  // sized once; append-only, offset = glyph.dataOffset
  size_t bmpUsed_ = 0;
  size_t bmpCap_ = 0;
  uint16_t maxGlyphs_ = 0;
  std::vector<uint32_t> cps_;
  std::vector<EpdGlyph> glyphs_;
  std::vector<EpdUnicodeInterval> intervals_;
  EpdFontData data_{};
  EpdFont font_{&data_};
};
