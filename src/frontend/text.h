// Draws text into XRGB8888 frames for the front end's own screens, with an
// embedded font (DejaVu Sans Bold, see tools/fontgen).
#pragma once

#include <cstdint>
#include <string>

namespace didj::frontend {

struct Frame {
    uint32_t* pixels;
    int width, height;
};

int text_height();
int text_width(const std::string& s);
// Draws s with its top-left corner at (x, y), blending into the frame.
void draw_text(Frame& f, int x, int y, const std::string& s, uint32_t rgb);
void fill_rect(Frame& f, int x, int y, int w, int h, uint32_t rgb);

}  // namespace didj::frontend
