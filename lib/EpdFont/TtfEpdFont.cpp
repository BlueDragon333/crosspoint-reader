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

constexpr uint32_t kReplacement = 0xFFFDu;

}  // namespace

bool TtfEpdFont::load(const uint8_t* ttfData, const uint32_t ttfLen, const uint16_t sizePx,
                      const size_t rasterCacheBytes, const size_t glyphCacheBytes, const uint16_t maxGlyphs) {
  loaded_ = false;
  ready_ = false;
  sizePx_ = sizePx;
  rasterBuf_.assign(rasterCacheBytes, 0);
  rasterArena_.init(rasterBuf_.data(), rasterBuf_.size());
  if (!ttf_.init(ttfData, ttfLen, rasterArena_)) return false;

  bmpCap_ = glyphCacheBytes;
  bmpArena_.assign(bmpCap_, 0);  // fixed capacity: data() is stable for data_.bitmap
  bmpUsed_ = 0;
  maxGlyphs_ = maxGlyphs;
  cps_.clear();
  cps_.reserve(maxGlyphs);       // reserved once so inserts never realloc
  glyphs_.clear();
  glyphs_.reserve(maxGlyphs);
  intervals_.clear();
  intervals_.reserve(maxGlyphs);
  loaded_ = true;
  refreshData();  // valid (empty) EpdFontData
  return true;
}

void TtfEpdFont::flushGlyphs() {
  cps_.clear();
  glyphs_.clear();
  intervals_.clear();
  bmpUsed_ = 0;
}

bool TtfEpdFont::ensureGlyph(const uint32_t cp) {
  // Resident already?
  const auto it = std::lower_bound(cps_.begin(), cps_.end(), cp);
  if (it != cps_.end() && *it == cp) return true;
  if (glyphs_.size() >= maxGlyphs_) return false;  // table full → caller flushes
  const size_t idx = static_cast<size_t>(it - cps_.begin());

  const int16_t advPx = ttf_.advance(cp, sizePx_, freeink::font::StyleNone);
  const freeink::font::GlyphBitmap* g = ttf_.rasterize(cp, sizePx_);

  EpdGlyph eg{};
  eg.advanceX = static_cast<uint16_t>((advPx < 0 ? 0 : advPx) << 4);  // 12.4 fixed-point px

  if (g != nullptr && g->width > 0 && g->height > 0 && g->pixels != nullptr) {
    const uint32_t px = static_cast<uint32_t>(g->width) * g->height;
    const size_t bytes = twoBit_ ? (px + 3) / 4 : (px + 7) / 8;
    if (bmpUsed_ + bytes > bmpCap_) return false;  // arena full → caller flushes
    uint8_t* dst = bmpArena_.data() + bmpUsed_;
    for (size_t i = 0; i < bytes; ++i) dst[i] = 0;  // arena is reused across flushes
    for (uint32_t i = 0; i < px; ++i) {
      const uint8_t a = g->pixels[i];  // 8-bit coverage, 0 = transparent
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
    // Space / zero-outline glyph: advance only, no bitmap.
    eg.dataOffset = static_cast<uint32_t>(bmpUsed_);
    eg.dataLength = 0;
  }

  // Insert keeping cps_/glyphs_ sorted-parallel. reserve() above guarantees no
  // reallocation, so data_.glyph stays valid; the shift only reorders entries
  // between render passes (never during one).
  cps_.insert(cps_.begin() + idx, cp);
  glyphs_.insert(glyphs_.begin() + idx, eg);
  return true;
}

void TtfEpdFont::refreshData() {
  // Coalesce sorted cps_ into intervals: getGlyph resolves
  // glyph[interval.offset + (cp - interval.first)], and glyphs_ is cp-sorted.
  intervals_.clear();
  for (size_t i = 0; i < cps_.size();) {
    size_t j = i;
    while (j + 1 < cps_.size() && cps_[j + 1] == cps_[j] + 1) ++j;
    EpdUnicodeInterval iv{};
    iv.first = cps_[i];
    iv.last = cps_[j];
    iv.offset = static_cast<uint32_t>(i);
    intervals_.push_back(iv);
    i = j + 1;
  }

  const int ascent = ttf_.ascent(sizePx_);
  const int lineHeight = ttf_.lineHeight(sizePx_);
  data_ = EpdFontData{};
  data_.bitmap = bmpArena_.empty() ? nullptr : bmpArena_.data();
  data_.glyph = glyphs_.empty() ? nullptr : glyphs_.data();
  data_.intervals = intervals_.empty() ? nullptr : intervals_.data();
  data_.intervalCount = static_cast<uint32_t>(intervals_.size());
  data_.advanceY = static_cast<uint8_t>(lineHeight > 255 ? 255 : (lineHeight < 0 ? 0 : lineHeight));
  data_.ascender = ascent;
  data_.descender = lineHeight - ascent > 0 ? lineHeight - ascent : 0;
  data_.is2Bit = twoBit_;
  ready_ = !glyphs_.empty();
}

bool TtfEpdFont::addCps(const std::vector<uint32_t>& want, const bool replace) {
  if (!loaded_) return false;
  if (replace) flushGlyphs();

  bool overflow = false;
  for (const uint32_t cp : want) {
    if (!ensureGlyph(cp)) {
      overflow = true;
      break;
    }
  }
  if (overflow) {
    // Cache full: flush and rebuild from just this request, bounding memory to
    // one request's glyphs. If the request itself exceeds the arena, add what
    // fits (best effort — enlarge the cache via load() for very dense pages).
    flushGlyphs();
    for (const uint32_t cp : want) {
      if (!ensureGlyph(cp)) break;
    }
  }
  refreshData();
  return ready_;
}

bool TtfEpdFont::build(const char* utf8, const bool twoBit) {
  if (!loaded_ || utf8 == nullptr) return false;
  twoBit_ = twoBit;
  std::vector<uint32_t> want;
  for (const char* p = utf8; *p != '\0';) {
    const uint32_t cp = nextCodepoint(p);
    if (cp != 0 && ttf_.hasGlyph(cp)) want.push_back(cp);
  }
  if (ttf_.hasGlyph(kReplacement)) want.push_back(kReplacement);
  return addCps(want, /*replace=*/true);
}

bool TtfEpdFont::build(const std::deque<std::string>& words, const bool includeHyphen, const bool twoBit) {
  if (!loaded_) return false;
  twoBit_ = twoBit;
  std::vector<uint32_t> want;
  for (const std::string& w : words) {
    for (const char* p = w.c_str(); *p != '\0';) {
      const uint32_t cp = nextCodepoint(p);
      if (cp != 0 && ttf_.hasGlyph(cp)) want.push_back(cp);
    }
  }
  if (includeHyphen && ttf_.hasGlyph('-')) want.push_back('-');
  if (ttf_.hasGlyph(kReplacement)) want.push_back(kReplacement);
  return addCps(want, /*replace=*/true);
}

bool TtfEpdFont::addCoverage(const char* utf8, const bool twoBit) {
  if (!loaded_ || utf8 == nullptr) return false;
  twoBit_ = twoBit;
  std::vector<uint32_t> want;
  for (const char* p = utf8; *p != '\0';) {
    const uint32_t cp = nextCodepoint(p);
    if (cp != 0 && ttf_.hasGlyph(cp)) want.push_back(cp);
  }
  return addCps(want, /*replace=*/false);
}

bool TtfEpdFont::addCoverage(const std::deque<std::string>& words, const bool includeHyphen, const bool twoBit) {
  if (!loaded_) return false;
  twoBit_ = twoBit;
  std::vector<uint32_t> want;
  for (const std::string& w : words) {
    for (const char* p = w.c_str(); *p != '\0';) {
      const uint32_t cp = nextCodepoint(p);
      if (cp != 0 && ttf_.hasGlyph(cp)) want.push_back(cp);
    }
  }
  if (includeHyphen && ttf_.hasGlyph('-')) want.push_back('-');
  return addCps(want, /*replace=*/false);
}
