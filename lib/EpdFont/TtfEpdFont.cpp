#include "TtfEpdFont.h"

#include <algorithm>

namespace {
uint32_t nextCodepoint(const char*& p) {
  const auto b0 = static_cast<uint8_t>(*p);
  if (b0 < 0x80) {
    ++p;
    return b0;
  }
  auto cont = [&](int i) { return static_cast<uint8_t>(p[i]) & 0x3F; };
  if ((b0 & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
    const uint32_t cp = ((b0 & 0x1Fu) << 6) | cont(1);
    p += 2;
    return cp;
  }
  if ((b0 & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
    const uint32_t cp = ((b0 & 0x0Fu) << 12) | (cont(1) << 6) | cont(2);
    p += 3;
    return cp;
  }
  if ((b0 & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
    const uint32_t cp = ((b0 & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
    p += 4;
    return cp;
  }
  ++p;
  return 0xFFFDu;
}
}  // namespace

bool TtfEpdFont::load(const uint8_t* ttfData, const uint32_t ttfLen, const uint16_t sizePx, const bool twoBit,
                      const size_t glyphCacheBytes, const uint16_t maxGlyphs) {
  loaded_ = false;
  sizePx_ = sizePx;
  // 0=regular(400) 1=bold(700) 2=italic(400,slant) 3=bold-italic(700,slant)
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    const int weight = (i & 1) ? 700 : 400;
    const bool italic = (i & 2) != 0;
    f.twoBit = twoBit;
    f.sizePx = sizePx;
    f.cap = glyphCacheBytes;
    f.maxGlyphs = maxGlyphs;
    f.bmp.assign(glyphCacheBytes, 0);
    f.used = 0;
    f.glyphs.clear();
    f.glyphs.reserve(maxGlyphs);
    f.cps.clear();
    f.cps.reserve(maxGlyphs);
    f.slot.clear();
    f.slot.reserve(maxGlyphs);
    f.ready = f.ft.init(ttfData, ttfLen, sizePx, weight, italic);
    setupFace(f);
  }
  if (!faces_[0].ready) return false;  // regular is mandatory
  loaded_ = true;
  return true;
}

void TtfEpdFont::setupFace(Face& f) const {
  const int ascent = f.ft.ascent(f.sizePx);
  const int lineHeight = f.ft.lineHeight(f.sizePx);
  f.data = EpdFontData{};
  f.data.advanceY = static_cast<uint8_t>(lineHeight > 255 ? 255 : (lineHeight < 0 ? 0 : lineHeight));
  f.data.ascender = ascent;
  f.data.descender = lineHeight - ascent > 0 ? lineHeight - ascent : 0;
  f.data.is2Bit = f.twoBit;
  f.data.glyphMissHandler = &TtfEpdFont::missThunk;
  f.data.glyphMissCtx = &f;
  f.data.coverageHandler = &TtfEpdFont::coverageThunk;
  f.data.vectorBitmapHandler = &TtfEpdFont::bitmapThunk;
  // f.font already points at f.data.
}

void TtfEpdFont::flushFace(Face& f) {
  f.glyphs.clear();
  f.cps.clear();
  f.slot.clear();
  f.used = 0;
}

const EpdGlyph* TtfEpdFont::faultGlyph(Face& f, const uint32_t cp) {
  if (!f.ready || !f.ft.hasGlyph(cp)) return nullptr;
  {
    const auto it = std::lower_bound(f.cps.begin(), f.cps.end(), cp);
    if (it != f.cps.end() && *it == cp) return &f.glyphs[f.slot[static_cast<size_t>(it - f.cps.begin())]];
  }

  const freeink::font::GlyphBitmap* g = f.ft.rasterize(cp, f.sizePx);
  const int16_t advPx = g ? g->advance : f.ft.advance(cp, f.sizePx, 0);
  uint32_t px = (g && g->pixels) ? static_cast<uint32_t>(g->width) * g->height : 0;
  size_t bytes = px ? (f.twoBit ? (px + 3) / 4 : (px + 7) / 8) : 0;
  if (bytes > f.cap) {
    bytes = 0;
    px = 0;
  }
  if (f.glyphs.size() >= f.maxGlyphs || f.used + bytes > f.cap) flushFace(f);

  EpdGlyph eg{};
  eg.advanceX = static_cast<uint16_t>((advPx < 0 ? 0 : advPx) << 4);
  if (px && g && g->pixels) {
    uint8_t* dst = f.bmp.data() + f.used;
    for (size_t i = 0; i < bytes; ++i) dst[i] = 0;
    for (uint32_t i = 0; i < px; ++i) {
      const uint8_t a = g->pixels[i];
      if (f.twoBit) {
        const uint8_t v = static_cast<uint8_t>((a * 3u + 127u) / 255u);
        dst[i >> 2] |= static_cast<uint8_t>(v << ((3 - (i & 3)) * 2));
      } else if (a >= 128) {
        dst[i >> 3] |= static_cast<uint8_t>(1u << (7 - (i & 7)));
      }
    }
    eg.width = static_cast<uint8_t>(g->width);
    eg.height = static_cast<uint8_t>(g->height);
    eg.left = g->xoff;
    eg.top = g->yoff;
    eg.dataOffset = static_cast<uint32_t>(f.used);
    eg.dataLength = static_cast<uint16_t>(bytes);
    f.used += bytes;
  } else {
    eg.dataOffset = static_cast<uint32_t>(f.used);
    eg.dataLength = 0;
  }

  f.glyphs.push_back(eg);
  const uint16_t newIdx = static_cast<uint16_t>(f.glyphs.size() - 1);
  const auto it = std::lower_bound(f.cps.begin(), f.cps.end(), cp);
  const size_t pos = static_cast<size_t>(it - f.cps.begin());
  f.cps.insert(it, cp);
  f.slot.insert(f.slot.begin() + pos, newIdx);
  return &f.glyphs[newIdx];
}

const EpdGlyph* TtfEpdFont::missThunk(void* ctx, const uint32_t codepoint) {
  return faultGlyph(*static_cast<Face*>(ctx), codepoint);
}
const uint8_t* TtfEpdFont::bitmapThunk(void* ctx, const EpdGlyph* glyph) {
  if (glyph == nullptr || glyph->dataLength == 0) return nullptr;
  return static_cast<Face*>(ctx)->bmp.data() + glyph->dataOffset;
}
bool TtfEpdFont::coverageThunk(void* ctx, const uint32_t codepoint) {
  return static_cast<Face*>(ctx)->ft.hasGlyph(codepoint);
}

EpdFontFamily TtfEpdFont::family() const {
  return EpdFontFamily(&faces_[0].font, faces_[1].ready ? &faces_[1].font : nullptr,
                       faces_[2].ready ? &faces_[2].font : nullptr, faces_[3].ready ? &faces_[3].font : nullptr);
}

// --- Optional pre-warm of the regular face -----------------------------------

bool TtfEpdFont::build(const char* utf8) {
  if (!loaded_) return false;
  flushFace(faces_[0]);
  return addCoverage(utf8);
}
bool TtfEpdFont::build(const std::deque<std::string>& words, const bool includeHyphen) {
  if (!loaded_) return false;
  flushFace(faces_[0]);
  return addCoverage(words, includeHyphen);
}
bool TtfEpdFont::addCoverage(const char* utf8) {
  if (!loaded_ || utf8 == nullptr) return false;
  for (const char* p = utf8; *p != '\0';) faultGlyph(faces_[0], nextCodepoint(p));
  return true;
}
bool TtfEpdFont::addCoverage(const std::deque<std::string>& words, const bool includeHyphen) {
  if (!loaded_) return false;
  for (const std::string& w : words) {
    for (const char* p = w.c_str(); *p != '\0';) faultGlyph(faces_[0], nextCodepoint(p));
  }
  if (includeHyphen) faultGlyph(faces_[0], '-');
  return true;
}
