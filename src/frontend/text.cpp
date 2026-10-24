#include "frontend/text.h"

#include <algorithm>

namespace didj::frontend {

namespace {

#include "frontend/font_data.inc"

const FontGlyph& glyph(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return kFontGlyphs[(u >= 32 && u < 127) ? u - 32 : '?' - 32];
}

uint32_t mix(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t d = (dst >> shift) & 0xff, s = (src >> shift) & 0xff;
        out |= ((s * a + d * (255 - a)) / 255) << shift;
    }
    return out;
}

}  // namespace

int text_height() { return kFontHeight; }

int text_width(const std::string& s) {
    int w = 0;
    for (char c : s) w += glyph(c).advance;
    return w;
}

void draw_text(Frame& f, int x, int y, const std::string& s, uint32_t rgb) {
    for (char c : s) {
        const FontGlyph& g = glyph(c);
        for (int gy = 0; gy < kFontHeight; ++gy) {
            const int py = y + gy;
            if (py < 0 || py >= f.height) continue;
            for (int gx = 0; gx < g.width; ++gx) {
                const int px = x + gx;
                if (px < 0 || px >= f.width) continue;
                const uint32_t a = kFontCoverage[g.offset + gy * g.width + gx];
                if (!a) continue;
                uint32_t& d = f.pixels[py * f.width + px];
                d = mix(d, rgb, a);
            }
        }
        x += g.advance;
    }
}

void fill_rect(Frame& f, int x, int y, int w, int h, uint32_t rgb) {
    for (int py = std::max(y, 0); py < std::min(y + h, f.height); ++py)
        for (int px = std::max(x, 0); px < std::min(x + w, f.width); ++px) f.pixels[py * f.width + px] = rgb;
}

}  // namespace didj::frontend
