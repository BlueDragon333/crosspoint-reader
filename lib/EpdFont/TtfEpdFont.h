#pragma once

// CrossPoint <- FreeInkFont adapter (FreeType, 4-style).
//
// Renders a TrueType/OpenType file — including OpenType VARIABLE fonts — as a
// CrossPoint EpdFontFamily with real regular / bold / italic / bold-italic:
//   * bold   = the font's `wght` variation axis (700 vs 400) when present
//   * italic = the `ital`/`slnt` axis when present, else an oblique shear
// via the FreeInkFont FreeType backend (freeink::font::FtFont). Static fonts
// with no axes render their single face for all styles (no synthesized bold).
//
// Each style is an independent, LAZILY-cached EpdFont: glyphs fault in on any
// draw (glyphMissHandler) and the bitmap comes from the font's own cache
// (vectorBitmapHandler), so every draw path works with no prewarm. Caches are
// fixed-size (bounded) and flush when full. family() hands GfxRenderer all four
// faces; getData(style) selects one.
//
// Lifetime: the TTF bytes are BORROWED (all four faces reference them) and must
// outlive this object; this object must outlive any GfxRenderer registration.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <FtFont.h>  // freeink::font::FtFont (FreeType backend)

#include "EpdFont.h"
#include "EpdFontData.h"
#include "EpdFontFamily.h"

class TtfEpdFont {
 public:
  // Borrow `ttfData` (must outlive this object) and build the four styled faces
  // at `sizePx`. `twoBit` = 4-level antialiased gray. Cache size is per-face.
  bool load(const uint8_t* ttfData, uint32_t ttfLen, uint16_t sizePx, bool twoBit = true,
            size_t glyphCacheBytes = 64 * 1024, uint16_t maxGlyphs = 1024);

  bool ready() const { return loaded_; }
  uint16_t sizePx() const { return sizePx_; }

  // All four styled faces (regular required; others null if the face failed).
  EpdFontFamily family() const;
  // Regular face only (single-style callers / setFallbackFont).
  const EpdFont* epdFont() const { return loaded_ ? &faces_[0].font : nullptr; }

  // Optional batch pre-warm of the REGULAR face (bold/italic fault lazily).
  bool build(const char* utf8);
  bool build(const std::deque<std::string>& words, bool includeHyphen);
  bool addCoverage(const char* utf8);
  bool addCoverage(const std::deque<std::string>& words, bool includeHyphen);

 private:
  // One styled face + its lazy packed-glyph cache + EpdFont view.
  struct Face {
    freeink::font::FtFont ft;
    std::vector<uint8_t> bmp;  // fixed capacity; glyph.dataOffset indexes it
    size_t used = 0;
    size_t cap = 0;
    uint16_t maxGlyphs = 0;
    bool twoBit = true;
    uint16_t sizePx = 0;
    bool ready = false;
    std::vector<EpdGlyph> glyphs;   // append-only; stable addresses
    std::vector<uint32_t> cps;      // sorted; parallel to slot
    std::vector<uint16_t> slot;     // glyph index for cps[i]
    EpdFontData data{};
    EpdFont font{&data};
  };

  static const EpdGlyph* missThunk(void* ctx, uint32_t codepoint);
  static const uint8_t* bitmapThunk(void* ctx, const EpdGlyph* glyph);
  static bool coverageThunk(void* ctx, uint32_t codepoint);
  static const EpdGlyph* faultGlyph(Face& f, uint32_t codepoint);
  static void flushFace(Face& f);
  void setupFace(Face& f) const;  // wire data handlers + metrics after ft.init

  Face faces_[4];  // 0=regular 1=bold 2=italic 3=bold-italic
  uint16_t sizePx_ = 0;
  bool loaded_ = false;
};
