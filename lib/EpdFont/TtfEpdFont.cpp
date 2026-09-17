#include "TtfEpdFont.h"

#include <algorithm>

namespace {

// Minimal UTF-8 decoder: returns the next codepoint and advances `p`. Invalid
// bytes decode as U+FFFD and advance one byte so the loop always progresses.
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
                      const size_t rasterCacheBytes, const size_t glyphCacheBytes, const uint16_t maxGlyphs) {
  loaded_ = false;
  sizePx_ = sizePx;
  twoBit_ = twoBit;
  rasterBuf_.assign(rasterCacheBytes, 0);
  rasterArena_.init(rasterBuf_.data(), rasterBuf_.size());
  if (!ttf_.init(ttfData, ttfLen, rasterArena_)) return false;

  bmpCap_ = glyphCacheBytes;
  bmpArena_.assign(bmpCap_, 0);  // fixed capacity: data() is stable for glyph bytes
  bmpUsed_ = 0;
  maxGlyphs_ = maxGlyphs;
  glyphs_.clear();
  glyphs_.reserve(maxGlyphs);  // reserved once → appended glyph addresses are stable
  cpsSorted_.clear();
  cpsSorted_.reserve(maxGlyphs);
  slotForCp_.clear();
  slotForCp_.reserve(maxGlyphs);

  const int ascent = ttf_.ascent(sizePx_);
  const int lineHeight = ttf_.lineHeight(sizePx_);
  data_ = EpdFontData{};
  // No static glyph/interval/bitmap tables: everything faults through the
  // handlers below (like an SD card font, minus the on-disk backing).
  data_.advanceY = static_cast<uint8_t>(lineHeight > 255 ? 255 : (lineHeight < 0 ? 0 : lineHeight));
  data_.ascender = ascent;
  data_.descender = lineHeight - ascent > 0 ? lineHeight - ascent : 0;
  data_.is2Bit = twoBit_;
  data_.glyphMissHandler = &TtfEpdFont::missThunk;
  data_.glyphMissCtx = this;
  data_.coverageHandler = &TtfEpdFont::coverageThunk;
  data_.vectorBitmapHandler = &TtfEpdFont::bitmapThunk;

  loaded_ = true;
  return true;
}

void TtfEpdFont::flushGlyphs() {
  glyphs_.clear();
  cpsSorted_.clear();
  slotForCp_.clear();
  bmpUsed_ = 0;
}

const EpdGlyph* TtfEpdFont::faultGlyph(const uint32_t cp) {
  if (!loaded_ || !ttf_.hasGlyph(cp)) return nullptr;

  // Already cached?
  {
    const auto it = std::lower_bound(cpsSorted_.begin(), cpsSorted_.end(), cp);
    if (it != cpsSorted_.end() && *it == cp) {
      return &glyphs_[slotForCp_[static_cast<size_t>(it - cpsSorted_.begin())]];
    }
  }

  const int16_t advPx = ttf_.advance(cp, sizePx_, freeink::font::StyleNone);
  const freeink::font::GlyphBitmap* g = ttf_.rasterize(cp, sizePx_);
  uint32_t px = (g != nullptr && g->pixels != nullptr) ? static_cast<uint32_t>(g->width) * g->height : 0;
  size_t bytes = px ? (twoBit_ ? (px + 3) / 4 : (px + 7) / 8) : 0;
  if (bytes > bmpCap_) {  // single glyph larger than the whole arena: store metrics only
    bytes = 0;
    px = 0;
  }

  // Make room. Flush BEFORE writing so bmpUsed_/tables are consistent.
  if (glyphs_.size() >= maxGlyphs_ || bmpUsed_ + bytes > bmpCap_) flushGlyphs();

  EpdGlyph eg{};
  eg.advanceX = static_cast<uint16_t>((advPx < 0 ? 0 : advPx) << 4);  // 12.4 fixed-point px
  if (px && g != nullptr && g->pixels != nullptr) {
    uint8_t* dst = bmpArena_.data() + bmpUsed_;
    for (size_t i = 0; i < bytes; ++i) dst[i] = 0;  // arena is reused across flushes
    for (uint32_t i = 0; i < px; ++i) {
      const uint8_t a = g->pixels[i];
      if (twoBit_) {
        const uint8_t v = static_cast<uint8_t>((a * 3u + 127u) / 255u);  // 0..3, 3 = black
        dst[i >> 2] |= static_cast<uint8_t>(v << ((3 - (i & 3)) * 2));
      } else if (a >= 128) {
        dst[i >> 3] |= static_cast<uint8_t>(1u << (7 - (i & 7)));
      }
    }
    eg.width = static_cast<uint8_t>(g->width);
    eg.height = static_cast<uint8_t>(g->height);
    eg.left = g->xoff;
    eg.top = g->yoff;
    eg.dataOffset = static_cast<uint32_t>(bmpUsed_);
    eg.dataLength = static_cast<uint16_t>(bytes);
    bmpUsed_ += bytes;
  } else {
    eg.dataOffset = static_cast<uint32_t>(bmpUsed_);
    eg.dataLength = 0;  // space / zero-outline / oversize glyph: advance only
  }

  glyphs_.push_back(eg);
  const uint16_t newIdx = static_cast<uint16_t>(glyphs_.size() - 1);
  // Insert into the sorted lookup (re-find: a flush above may have cleared it).
  const auto it = std::lower_bound(cpsSorted_.begin(), cpsSorted_.end(), cp);
  const size_t pos = static_cast<size_t>(it - cpsSorted_.begin());
  cpsSorted_.insert(it, cp);
  slotForCp_.insert(slotForCp_.begin() + pos, newIdx);
  return &glyphs_[newIdx];
}

const EpdGlyph* TtfEpdFont::missThunk(void* ctx, const uint32_t codepoint) {
  return static_cast<TtfEpdFont*>(ctx)->faultGlyph(codepoint);
}

const uint8_t* TtfEpdFont::bitmapThunk(void* ctx, const EpdGlyph* glyph) {
  if (glyph == nullptr || glyph->dataLength == 0) return nullptr;  // zero-width glyph
  return static_cast<TtfEpdFont*>(ctx)->bmpArena_.data() + glyph->dataOffset;
}

bool TtfEpdFont::coverageThunk(void* ctx, const uint32_t codepoint) {
  return static_cast<TtfEpdFont*>(ctx)->ttf_.hasGlyph(codepoint);
}

// --- Optional batch pre-warm --------------------------------------------------

bool TtfEpdFont::build(const char* utf8) {
  if (!loaded_) return false;
  flushGlyphs();
  return addCoverage(utf8);
}

bool TtfEpdFont::build(const std::deque<std::string>& words, const bool includeHyphen) {
  if (!loaded_) return false;
  flushGlyphs();
  return addCoverage(words, includeHyphen);
}

bool TtfEpdFont::addCoverage(const char* utf8) {
  if (!loaded_ || utf8 == nullptr) return false;
  for (const char* p = utf8; *p != '\0';) faultGlyph(nextCodepoint(p));
  return true;
}

bool TtfEpdFont::addCoverage(const std::deque<std::string>& words, const bool includeHyphen) {
  if (!loaded_) return false;
  for (const std::string& w : words) {
    for (const char* p = w.c_str(); *p != '\0';) faultGlyph(nextCodepoint(p));
  }
  if (includeHyphen) faultGlyph('-');
  return true;
}
