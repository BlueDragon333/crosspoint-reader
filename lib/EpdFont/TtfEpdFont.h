#pragma once

// CrossPoint <- FreeInkFont adapter (FreeType, 4-style, lazy, streaming).
//
// Renders a TrueType/OpenType file — including OpenType VARIABLE fonts — as a
// CrossPoint EpdFontFamily with real regular / bold / italic / bold-italic
// (bold = wght axis; italic = ital/slnt axis or oblique shear) via the
// FreeInkFont FreeType backend.
//
// Memory-lean:
//   * STREAMING (loadStream) — FreeType pulls bytes from SD on demand, so a
//     multi-MB variable/CJK file never sits in RAM (only the tables + glyphs
//     used). Use load() only for small fonts you already hold in RAM.
//   * LAZY per-style faces — only the regular face is built up front; bold /
//     italic / bold-italic (and their glyph caches) are created on first use,
//     so a book with no bold pays nothing for it, and UI fallbacks that only
//     draw regular cost one face.
//   * bounded per-face glyph caches that flush when full.
//
// Lifetime: for load(), the bytes are borrowed; for loadStream(), the read
// source (e.g. an open SD file) is borrowed. Either must outlive this object,
// which must outlive any GfxRenderer registration.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <FtFont.h>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "EpdFontFamily.h"

class TtfEpdFont {
 public:
  // Resident source (small fonts held in RAM). Bytes borrowed.
  bool load(const uint8_t* ttfData, uint32_t ttfLen, uint16_t sizePx, bool twoBit = true,
            size_t glyphCacheBytes = 32 * 1024, uint16_t maxGlyphs = 768);
  // Streamed source (big fonts): FreeType reads via `read`/`ctx` (source borrowed).
  bool loadStream(freeink::font::FtFont::ReadFn read, void* ctx, unsigned long fileSize, uint16_t sizePx,
                  bool twoBit = true, size_t glyphCacheBytes = 32 * 1024, uint16_t maxGlyphs = 768);

  bool ready() const { return loaded_; }
  uint16_t sizePx() const { return sizePx_; }

  EpdFontFamily family() const;
  const EpdFont* epdFont() const { return loaded_ ? &faces_[0].font : nullptr; }

  // Optional batch pre-warm of the REGULAR face (other styles fault lazily).
  bool build(const char* utf8);
  bool build(const std::deque<std::string>& words, bool includeHyphen);
  bool addCoverage(const char* utf8);
  bool addCoverage(const std::deque<std::string>& words, bool includeHyphen);

 private:
  struct Face {
    TtfEpdFont* owner = nullptr;
    freeink::font::FtFont ft;
    std::vector<uint8_t> bmp;
    size_t used = 0;
    size_t cap = 0;
    uint16_t maxGlyphs = 0;
    bool twoBit = true;
    uint16_t sizePx = 0;
    int weight = 400;
    bool italic = false;
    bool inited = false;  // init attempted (lazy)
    bool ready = false;   // FreeType face live
    std::vector<EpdGlyph> glyphs;
    std::vector<uint32_t> cps;
    std::vector<uint16_t> slot;
    EpdFontData data{};
    EpdFont font{&data};
  };

  static const EpdGlyph* missThunk(void* ctx, uint32_t codepoint);
  static const uint8_t* bitmapThunk(void* ctx, const EpdGlyph* glyph);
  static bool coverageThunk(void* ctx, uint32_t codepoint);

  bool commonLoad(uint16_t sizePx, bool twoBit, size_t glyphCacheBytes, uint16_t maxGlyphs);
  void initFace(Face& f);       // lazy: create the FT face + caches on first use
  void setupFace(Face& f);      // wire data handlers + metrics
  const EpdGlyph* faultGlyph(Face& f, uint32_t codepoint);
  static void flushFace(Face& f);

  // Source (one of the two forms).
  bool streamed_ = false;
  const uint8_t* data_ = nullptr;
  uint32_t len_ = 0;
  freeink::font::FtFont::ReadFn read_ = nullptr;
  void* ctx_ = nullptr;
  unsigned long fileSize_ = 0;

  Face faces_[4];  // 0=regular 1=bold 2=italic 3=bold-italic
  uint16_t sizePx_ = 0;
  bool loaded_ = false;
};
