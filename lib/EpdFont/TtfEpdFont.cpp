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
  streamed_ = false;
  data_ = ttfData;
  len_ = ttfLen;
  if (data_ == nullptr || len_ == 0) return false;
  return commonLoad(sizePx, twoBit, glyphCacheBytes, maxGlyphs);
}

bool TtfEpdFont::loadStream(const freeink::font::FtFont::ReadFn read, void* ctx, const unsigned long fileSize,
                            const uint16_t sizePx, const bool twoBit, const size_t glyphCacheBytes,
                            const uint16_t maxGlyphs) {
  streamed_ = true;
  read_ = read;
  ctx_ = ctx;
  fileSize_ = fileSize;
  if (read_ == nullptr || fileSize_ == 0) return false;
  return commonLoad(sizePx, twoBit, glyphCacheBytes, maxGlyphs);
}

bool TtfEpdFont::commonLoad(const uint16_t pointSize, const bool twoBit, const size_t glyphCacheBytes,
                            const uint16_t maxGlyphs) {
  loaded_ = false;
  // CrossPoint speaks point-size-at-150-DPI (matching the .cpfont converter's
  // FT_Set_Char_Size(size, size, 150, 150)); FreeInkFont speaks pixels. Convert
  // so vector fonts match the on-glyph size and metrics of the bitmap fonts:
  //   ppem = pointSize * 150 / 72.
  const uint16_t sizePx = static_cast<uint16_t>((static_cast<uint32_t>(pointSize) * 150u + 36u) / 72u);
  sizePx_ = sizePx;
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    f.owner = this;
    f.weight = (i & 1) ? 700 : 400;
    f.italic = (i & 2) != 0;
    f.twoBit = twoBit;
    f.sizePx = sizePx;
    f.cap = glyphCacheBytes;
    f.maxGlyphs = maxGlyphs;
    f.inited = false;
    f.ready = false;
    f.used = 0;
  }
  initFace(faces_[0]);           // regular eagerly: validates the font + gives metrics
  if (!faces_[0].ready) return false;
  for (int i = 1; i < 4; ++i) setupFace(faces_[i]);  // handlers + placeholder metrics (regular's)
  loaded_ = true;
  return true;
}

void TtfEpdFont::initFace(Face& f) {
  if (f.inited) return;
  f.inited = true;
  if (streamed_) {
    f.ready = f.ft.initStream(read_, ctx_, fileSize_, f.sizePx, f.weight, f.italic);
  } else {
    f.ready = f.ft.init(data_, len_, f.sizePx, f.weight, f.italic);
  }
  // Caches are NOT pre-reserved: the byte arena (f.bmp) and the glyph tables
  // grow on demand in faultGlyph and converge on the book's page needs (see the
  // header's memory note). f.cap is only the hard ceiling that triggers a flush.
  setupFace(f);
}

void TtfEpdFont::setupFace(Face& f) {
  // Metrics from this face once live, else borrow the (always-live) regular
  // face's — same font/size, so a fine placeholder until this face is faulted.
  freeink::font::FtFont& src = f.ready ? f.ft : faces_[0].ft;
  const int ascent = src.ascent(f.sizePx);
  const int lineHeight = src.lineHeight(f.sizePx);
  f.data = EpdFontData{};
  f.data.advanceY = static_cast<uint8_t>(lineHeight > 255 ? 255 : (lineHeight < 0 ? 0 : lineHeight));
  f.data.ascender = ascent;
  f.data.descender = lineHeight - ascent > 0 ? lineHeight - ascent : 0;
  f.data.is2Bit = f.twoBit;
  f.data.glyphMissHandler = &TtfEpdFont::missThunk;
  f.data.glyphMissCtx = &f;
  f.data.coverageHandler = &TtfEpdFont::coverageThunk;
  f.data.vectorBitmapHandler = &TtfEpdFont::bitmapThunk;
}

void TtfEpdFont::flushFace(Face& f) {
  f.glyphs.clear();
  f.cps.clear();
  f.slot.clear();
  f.used = 0;
}

void TtfEpdFont::clearCache() {
  // Drop cached page glyphs on every inited face but keep the allocations: the
  // vectors keep capacity (clear() does not free) and f.bmp keeps its buffer, so
  // the next prewarm re-faults into buffers already sized to the book.
  for (Face& f : faces_) {
    if (f.inited) flushFace(f);
  }
}

void TtfEpdFont::releaseResidentCaches() {
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    // Actually RELEASE the caches (swap-with-empty frees capacity; clear() alone
    // would not).
    flushFace(f);
    std::vector<uint8_t>().swap(f.bmp);
    std::vector<EpdGlyph>().swap(f.glyphs);
    std::vector<uint32_t>().swap(f.cps);
    std::vector<uint16_t>().swap(f.slot);
    // Keep the regular face's FreeType face live so coverage()/metrics still
    // answer without a reload (mirrors SD keeping its interval table resident).
    // Shed the lazy bold/italic/bold-italic faces entirely; they re-init on the
    // next glyph fault for that style.
    if (i == 0) continue;
    if (f.inited) {
      f.ft.deinit();
      f.inited = false;
      f.ready = false;
    }
  }
}

const EpdGlyph* TtfEpdFont::faultGlyph(Face& f, const uint32_t cp) {
  if (!f.inited) initFace(f);
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
    // Grow the byte arena on demand toward the book's page high-water mark.
    // flush above guarantees f.used + bytes <= f.cap, so this never exceeds the
    // ceiling; capacity is retained across page turns (clearCache keeps it).
    if (f.bmp.size() < f.used + bytes) f.bmp.resize(f.used + bytes, 0);
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
    // FreeInkFont's yoff is negative-above (top offset from baseline); CrossPoint's
    // EpdGlyph.top is positive-above (renderer computes screen Y = cursorY - top,
    // matching the .cpfont converter's top = FreeType bitmap_top). Flip the sign.
    eg.top = static_cast<int16_t>(-g->yoff);
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
  Face* f = static_cast<Face*>(ctx);
  return f->owner->faultGlyph(*f, codepoint);
}
const uint8_t* TtfEpdFont::bitmapThunk(void* ctx, const EpdGlyph* glyph) {
  if (glyph == nullptr || glyph->dataLength == 0) return nullptr;
  return static_cast<Face*>(ctx)->bmp.data() + glyph->dataOffset;
}
bool TtfEpdFont::coverageThunk(void* ctx, const uint32_t codepoint) {
  // All styles share one file → coverage comes from the always-live regular face.
  return static_cast<Face*>(ctx)->owner->faces_[0].ft.hasGlyph(codepoint);
}

EpdFontFamily TtfEpdFont::family() const {
  return EpdFontFamily(&faces_[0].font, &faces_[1].font, &faces_[2].font, &faces_[3].font);
}

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
