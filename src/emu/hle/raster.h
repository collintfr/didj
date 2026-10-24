// A software rasterizer with the fixed-function pipeline of OpenGL ES 1.1,
// after vertex processing: clipping, perspective-correct interpolation,
// texturing with the texture environment, alpha test, depth test, blending.
//
// The Didj's 3D core drew 320x240, so the host CPU keeps up easily, and the
// results are the same on every host.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace didj::emu::hle {

// Pixels are RGBA8, R in the low byte.
struct Texture {
    int width = 0, height = 0;
    std::vector<uint32_t> texels;
    uint32_t min_filter = 0x2702;  // GL_NEAREST_MIPMAP_LINEAR
    uint32_t mag_filter = 0x2601;  // GL_LINEAR
    uint32_t wrap_s = 0x2901, wrap_t = 0x2901;  // GL_REPEAT
};

struct Vertex {
    float pos[4];    // clip coordinates
    float color[4];  // 0..1
    float tex[2];
};

struct RasterState {
    int vp_x = 0, vp_y = 0, vp_w = 320, vp_h = 240;
    float depth_near = 0, depth_far = 1;
    bool scissor = false;
    int sc_x = 0, sc_y = 0, sc_w = 320, sc_h = 240;
    bool cull = false;
    uint32_t cull_face = 0x0405;  // GL_BACK
    bool front_ccw = true;
    bool depth_test = false, depth_write = true;
    uint32_t depth_func = 0x0201;  // GL_LESS
    bool blend = false;
    uint32_t blend_src = 1, blend_dst = 0;  // GL_ONE, GL_ZERO
    bool alpha_test = false;
    uint32_t alpha_func = 0x0207;  // GL_ALWAYS
    float alpha_ref = 0;
    bool color_write[4] = {true, true, true, true};
    const Texture* texture = nullptr;  // null: texturing off
    uint32_t tex_env = 0x2100;         // GL_MODULATE
    float tex_env_color[4] = {0, 0, 0, 0};
    bool flat = false;
    float point_size = 1, line_width = 1;
};

class Raster {
public:
    Raster(int width, int height);
    void resize(int width, int height);
    int width() const { return w_; }
    int height() const { return h_; }

    // clear_color is RGBA 0..1; honors scissor and color mask like glClear.
    void clear(bool color, bool depth, const float clear_color[4], float clear_depth, const RasterState& s);

    void triangle(const Vertex& a, const Vertex& b, const Vertex& c, const RasterState& s);
    void line(const Vertex& a, const Vertex& b, const RasterState& s);
    void point(const Vertex& a, const RasterState& s);

    // Row 0 is the top of the picture.
    const uint32_t* pixels() const { return color_.data(); }
    uint32_t pixel(int x, int y) const { return color_[size_t(y) * size_t(w_) + size_t(x)]; }

private:
    struct WinVertex {
        float x, y, z, inv_w;
        float color[4];  // divided by w
        float tex[2];    // divided by w
    };
    void draw_clipped(std::vector<Vertex>& poly, const RasterState& s);
    WinVertex to_window(const Vertex& v, const RasterState& s) const;
    void fill(const WinVertex& a, const WinVertex& b, const WinVertex& c, const RasterState& s, const float* flat);
    void shade(int x, int y, float z, const float color[4], const float tex[2], const RasterState& s);
    void bounds(const RasterState& s, int& x0, int& y0, int& x1, int& y1) const;

    int w_, h_;
    std::vector<uint32_t> color_;
    std::vector<float> depth_;
};

// Samples t at (s, t) with its filters and wrap modes; RGBA 0..1.
void sample_texture(const Texture& tex, float s, float t, float out[4]);

}  // namespace didj::emu::hle
