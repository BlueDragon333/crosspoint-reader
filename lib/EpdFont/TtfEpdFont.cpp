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

void TtfEpdFont::addResidentSource(const uint8_t style, const uint8_t* data, const uint32_t len) {
  if (style >= 4 || data == nullptr || len == 0) return;
  Source& s = sources_[style];
  s = Source{};
  s.present = true;
  s.streamed = false;
  s.data = data;
  s.len = len;
}

void TtfEpdFont::addStreamSource(const uint8_t style, const freeink::font::FtFont::ReadFn read, void* ctx,
                                 const unsigned long fileSize) {
  if (style >= 4 || read == nullptr || fileSize == 0) return;
  Source& s = sources_[style];
  s = Source{};
  s.present = true;
  s.streamed = true;
  s.read = read;
  s.ctx = ctx;
  s.fileSize = fileSize;
}

void TtfEpdFont::resolveFaces() {
  // Map each style face onto the best available source. Regular (0) anchors the
  // family; the others prefer an explicit file, then fall back to synthesizing
  // from a related source (wght axis / faux bold for weight, ital axis / oblique
  // for slant — all handled inside FtFont).
  const bool haveB = sources_[Bold].present;
  const bool haveI = sources_[Italic].present;
  const bool haveBI = sources_[BoldItalic].present;

  // regular
  faces_[Regular].srcIndex = Regular;
  faces_[Regular].weight = 400;
  faces_[Regular].wantItalic = false;

  // bold
  faces_[Bold].srcIndex = haveB ? Bold : Regular;
  faces_[Bold].weight = haveB ? 400 : 700;
  faces_[Bold].wantItalic = false;

  // italic
  faces_[Italic].srcIndex = haveI ? Italic : Regular;
  faces_[Italic].weight = 400;
  faces_[Italic].wantItalic = !haveI;  // oblique/axis only when using the roman source

  // bold-italic: dedicated file > bold-of-italic-file > italic-of-bold-file > roman
  Face& bi = faces_[BoldItalic];
  if (haveBI) {
    bi.srcIndex = BoldItalic;
    bi.weight = 400;
    bi.wantItalic = false;
  } else if (haveI) {
    bi.srcIndex = Italic;
    bi.weight = 700;  // wght axis / faux bold on the italic design
    bi.wantItalic = false;
  } else if (haveB) {
    bi.srcIndex = Bold;
    bi.weight = 400;
    bi.wantItalic = true;  // oblique on the bold design
  } else {
    bi.srcIndex = Regular;
    bi.weight = 700;
    bi.wantItalic = true;
  }
}

bool TtfEpdFont::load(const uint16_t pointSize, const bool twoBit, const size_t glyphCacheBytes,
                      const uint16_t maxGlyphs) {
  loaded_ = false;
  if (!sources_[Regular].present) return false;
  // CrossPoint speaks point-size-at-150-DPI (matching the .cpfont converter's
  // FT_Set_Char_Size(size, size, 150, 150)); FreeInkFont speaks pixels. Convert
  // so vector fonts match the on-glyph size and metrics of the bitmap fonts:
  //   ppem = pointSize * 150 / 72.
  const uint16_t sizePx = static_cast<uint16_t>((static_cast<uint32_t>(pointSize) * 150u + 36u) / 72u);
  sizePx_ = sizePx;
  resolveFaces();
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    f.owner = this;
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
  const Source& s = sources_[f.srcIndex];
  if (!s.present) {
    f.ready = false;
  } else if (s.streamed) {
    f.ready = f.ft.initStream(s.read, s.ctx, s.fileSize, f.sizePx, f.weight, f.wantItalic);
  } else {
    f.ready = f.ft.init(s.data, s.len, f.sizePx, f.weight, f.wantItalic);
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
    freeink::font::PsramVector<uint8_t>().swap(f.bmp);
    freeink::font::PsramVector<EpdGlyph>().swap(f.glyphs);
    freeink::font::PsramVector<uint32_t>().swap(f.cps);
    freeink::font::PsramVector<uint16_t>().swap(f.slot);
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
