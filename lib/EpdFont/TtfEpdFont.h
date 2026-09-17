#pragma once

// CrossPoint <- FreeInkFont adapter (lazy).
//
// Renders TrueType/OpenType through the FreeInkFont engine (freeink::font, an
// stb_truetype backend) and exposes it as a normal CrossPoint EpdFont, so the
// existing GfxRenderer / EpdFont draw path renders TTF text with NO changes to
// layout or rendering.
//
// Glyph model: LAZY + bounded, exactly like SD card fonts. The EpdFontData
// carries a glyphMissHandler (getGlyph faults a codepoint in on demand),
// a vectorBitmapHandler (getGlyphBitmap returns the cached bytes), and a
// coverageHandler (hasCodepoint for UI script fallback). Glyphs are rasterized
// once into a FIXED pre-allocated arena and cached; when the arena/table fills
// it flushes and refills. Because faulting happens inside getGlyph, EVERY draw
// path works — reader, settings preview, UI, menus — with no per-path prewarm.
//
// build()/addCoverage() are OPTIONAL batch pre-warms (fault a page's glyphs up
// front so the first draw isn't a rasterization burst); rendering is correct
// without them.
//
// Lifetime: the TTF bytes are BORROWED (PSRAM / resident heap) and must outlive
// this object; this object must outlive any GfxRenderer registration.

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
  // `sizePx`. `twoBit` = 4-level antialiased gray (false = 1-bit). The caches
  // bound memory: `glyphCacheBytes`/`maxGlyphs` hold one page's glyphs.
  bool load(const uint8_t* ttfData, uint32_t ttfLen, uint16_t sizePx, bool twoBit = true,
            size_t rasterCacheBytes = 48 * 1024, size_t glyphCacheBytes = 96 * 1024, uint16_t maxGlyphs = 1536);

  // Optional batch pre-warm: fault the text's glyphs now. build() clears the
  // cache first; addCoverage() adds. Rendering works without these.
  bool build(const char* utf8);
  bool build(const std::deque<std::string>& words, bool includeHyphen);
  bool addCoverage(const char* utf8);
  bool addCoverage(const std::deque<std::string>& words, bool includeHyphen);

  bool ready() const { return loaded_; }
  uint16_t sizePx() const { return sizePx_; }

  // Renderer-facing handles. The EpdFont is a stable, always-valid object once
  // loaded (glyphs fault in behind it); valid while this object is alive.
  const EpdFont* epdFont() const { return loaded_ ? &font_ : nullptr; }
  EpdFontFamily family() const { return EpdFontFamily(epdFont()); }

 private:
  // EpdFontData handler trampolines (ctx = this).
  static const EpdGlyph* missThunk(void* ctx, uint32_t codepoint);
  static const uint8_t* bitmapThunk(void* ctx, const EpdGlyph* glyph);
  static bool coverageThunk(void* ctx, uint32_t codepoint);

  // Rasterize + cache one glyph and return its (stable) EpdGlyph, or nullptr if
  // the font doesn't cover it. Flushes the cache first if it is full.
  const EpdGlyph* faultGlyph(uint32_t codepoint);
  void flushGlyphs();

  freeink::font::TtfFont ttf_;
  std::vector<uint8_t> rasterBuf_;  // FreeInkFont rasterizer scratch
  freeink::font::Arena rasterArena_;
  uint16_t sizePx_ = 0;
  bool loaded_ = false;
  bool twoBit_ = true;

  // Bounded, append-only packed-glyph cache. glyphs_ addresses are stable
  // (reserved once, append-only). cpsSorted_/slotForCp_ are a sorted lookup.
  std::vector<uint8_t> bmpArena_;  // fixed capacity; glyph.dataOffset indexes it
  size_t bmpUsed_ = 0;
  size_t bmpCap_ = 0;
  uint16_t maxGlyphs_ = 0;
  std::vector<EpdGlyph> glyphs_;
  std::vector<uint32_t> cpsSorted_;
  std::vector<uint16_t> slotForCp_;

  EpdFontData data_{};
  EpdFont font_{&data_};
};
