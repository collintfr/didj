#include "emu/hle/raster.h"

#include <algorithm>
#include <cmath>

namespace didj::emu::hle {

namespace {

// GL enums used here.
constexpr uint32_t kNever = 0x0200, kLess = 0x0201, kEqual = 0x0202, kLequal = 0x0203, kGreater = 0x0204,
                   kNotequal = 0x0205, kGequal = 0x0206;
constexpr uint32_t kFront = 0x0404, kBack = 0x0405, kFrontAndBack = 0x0408;
constexpr uint32_t kNearest = 0x2600, kNearestMipmapNearest = 0x2700, kNearestMipmapLinear = 0x2702;
constexpr uint32_t kClamp = 0x2900, kClampToEdge = 0x812f;
constexpr uint32_t kModulate = 0x2100, kDecal = 0x2101, kBlend = 0x0be2, kReplace = 0x1e01, kAdd = 0x0104;

bool compare(uint32_t func, float a, float b) {
    switch (func) {
        case kNever: return false;
        case kLess: return a < b;
        case kEqual: return a == b;
        case kLequal: return a <= b;
        case kGreater: return a > b;
        case kNotequal: return a != b;
        case kGequal: return a >= b;
        default: return true;
    }
}

void unpack(uint32_t p, float out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = float((p >> (8 * i)) & 0xff) * (1.0f / 255.0f);
}

uint32_t pack(const float c[4]) {
    uint32_t p = 0;
    for (int i = 0; i < 4; ++i) {
        float v = std::clamp(c[i], 0.0f, 1.0f);
        p |= uint32_t(std::lround(v * 255.0f)) << (8 * i);
    }
    return p;
}

void blend_factor(uint32_t f, const float src[4], const float dst[4], float out[4]) {
    switch (f) {
        case 0: for (int i = 0; i < 4; ++i) out[i] = 0; break;
        case 1: for (int i = 0; i < 4; ++i) out[i] = 1; break;
        case 0x0300: for (int i = 0; i < 4; ++i) out[i] = src[i]; break;          // SRC_COLOR
        case 0x0301: for (int i = 0; i < 4; ++i) out[i] = 1 - src[i]; break;      // ONE_MINUS_SRC_COLOR
        case 0x0302: for (int i = 0; i < 4; ++i) out[i] = src[3]; break;          // SRC_ALPHA
        case 0x0303: for (int i = 0; i < 4; ++i) out[i] = 1 - src[3]; break;      // ONE_MINUS_SRC_ALPHA
        case 0x0304: for (int i = 0; i < 4; ++i) out[i] = dst[3]; break;          // DST_ALPHA
        case 0x0305: for (int i = 0; i < 4; ++i) out[i] = 1 - dst[3]; break;      // ONE_MINUS_DST_ALPHA
        case 0x0306: for (int i = 0; i < 4; ++i) out[i] = dst[i]; break;          // DST_COLOR
        case 0x0307: for (int i = 0; i < 4; ++i) out[i] = 1 - dst[i]; break;      // ONE_MINUS_DST_COLOR
        case 0x0308: {                                                           // SRC_ALPHA_SATURATE
            float f2 = std::min(src[3], 1 - dst[3]);
            out[0] = out[1] = out[2] = f2;
            out[3] = 1;
            break;
        }
        default: for (int i = 0; i < 4; ++i) out[i] = (f == 1); break;
    }
}

int wrap(int i, int n, uint32_t mode) {
    if (mode == kClamp || mode == kClampToEdge) return std::clamp(i, 0, n - 1);
    i %= n;
    return i < 0 ? i + n : i;
}

}  // namespace

void sample_texture(const Texture& tex, float s, float t, float out[4]) {
    if (tex.width <= 0 || tex.height <= 0 || tex.texels.empty()) {
        out[0] = out[1] = out[2] = out[3] = 1;
        return;
    }
    const float u = s * float(tex.width), v = t * float(tex.height);
    // Mipmaps are not kept, so the minification filter only picks nearest or linear.
    const bool nearest = tex.mag_filter == kNearest &&
                         (tex.min_filter == kNearest || tex.min_filter == kNearestMipmapNearest ||
                          tex.min_filter == kNearestMipmapLinear);
    auto texel = [&](int x, int y) {
        x = wrap(x, tex.width, tex.wrap_s);
        y = wrap(y, tex.height, tex.wrap_t);
        return tex.texels[size_t(y) * size_t(tex.width) + size_t(x)];
    };
    if (nearest) {
        unpack(texel(int(std::floor(u)), int(std::floor(v))), out);
        return;
    }
    const float fu = u - 0.5f, fv = v - 0.5f;
    const int x0 = int(std::floor(fu)), y0 = int(std::floor(fv));
    const float ax = fu - float(x0), ay = fv - float(y0);
    float c00[4], c10[4], c01[4], c11[4];
    unpack(texel(x0, y0), c00);
    unpack(texel(x0 + 1, y0), c10);
    unpack(texel(x0, y0 + 1), c01);
    unpack(texel(x0 + 1, y0 + 1), c11);
    for (int i = 0; i < 4; ++i)
        out[i] = (c00[i] * (1 - ax) + c10[i] * ax) * (1 - ay) + (c01[i] * (1 - ax) + c11[i] * ax) * ay;
}

Raster::Raster(int width, int height) { resize(width, height); }

void Raster::resize(int width, int height) {
    w_ = std::max(width, 1);
    h_ = std::max(height, 1);
    color_.assign(size_t(w_) * size_t(h_), 0xff000000);
    depth_.assign(size_t(w_) * size_t(h_), 1.0f);
}

void Raster::bounds(const RasterState& s, int& x0, int& y0, int& x1, int& y1) const {
    x0 = 0, y0 = 0, x1 = w_, y1 = h_;  // GL window coordinates, y up; exclusive
    if (s.scissor) {
        x0 = std::max(x0, s.sc_x);
        y0 = std::max(y0, s.sc_y);
        x1 = std::min(x1, s.sc_x + s.sc_w);
        y1 = std::min(y1, s.sc_y + s.sc_h);
    }
}

void Raster::clear(bool color, bool depth, const float clear_color[4], float clear_depth, const RasterState& s) {
    int x0, y0, x1, y1;
    bounds(s, x0, y0, x1, y1);
    const uint32_t c = pack(clear_color);
    uint32_t keep = 0;
    for (int i = 0; i < 4; ++i)
        if (!s.color_write[i]) keep |= 0xffu << (8 * i);
    for (int y = y0; y < y1; ++y) {
        const size_t row = size_t(h_ - 1 - y) * size_t(w_);
        for (int x = x0; x < x1; ++x) {
            if (color) color_[row + size_t(x)] = (color_[row + size_t(x)] & keep) | (c & ~keep);
            if (depth) depth_[row + size_t(x)] = std::clamp(clear_depth, 0.0f, 1.0f);
        }
    }
}

Raster::WinVertex Raster::to_window(const Vertex& v, const RasterState& s) const {
    WinVertex o;
    const float iw = 1.0f / v.pos[3];
    o.x = float(s.vp_x) + (v.pos[0] * iw + 1) * 0.5f * float(s.vp_w);
    o.y = float(s.vp_y) + (v.pos[1] * iw + 1) * 0.5f * float(s.vp_h);
    o.z = s.depth_near + (v.pos[2] * iw + 1) * 0.5f * (s.depth_far - s.depth_near);
    o.inv_w = iw;
    for (int i = 0; i < 4; ++i) o.color[i] = v.color[i] * iw;
    o.tex[0] = v.tex[0] * iw;
    o.tex[1] = v.tex[1] * iw;
    return o;
}

void Raster::triangle(const Vertex& a, const Vertex& b, const Vertex& c, const RasterState& s) {
    std::vector<Vertex> poly = {a, b, c};
    draw_clipped(poly, s);
}

void Raster::draw_clipped(std::vector<Vertex>& poly, const RasterState& s) {
    // Flat shading takes the color of the last vertex of the primitive.
    float flat[4];
    for (int i = 0; i < 4; ++i) flat[i] = poly.back().color[i];

    // Sutherland-Hodgman against the six clip planes.
    for (int plane = 0; plane < 6 && !poly.empty(); ++plane) {
        const int axis = plane / 2;
        const float sign = (plane & 1) ? -1.0f : 1.0f;
        auto dist = [&](const Vertex& v) { return v.pos[3] + sign * v.pos[axis]; };
        std::vector<Vertex> out;
        for (size_t i = 0; i < poly.size(); ++i) {
            const Vertex& p = poly[i];
            const Vertex& q = poly[(i + 1) % poly.size()];
            const float dp = dist(p), dq = dist(q);
            if (dp >= 0) out.push_back(p);
            if ((dp >= 0) != (dq >= 0)) {
                const float t = dp / (dp - dq);
                Vertex m;
                for (int k = 0; k < 4; ++k) m.pos[k] = p.pos[k] + (q.pos[k] - p.pos[k]) * t;
                for (int k = 0; k < 4; ++k) m.color[k] = p.color[k] + (q.color[k] - p.color[k]) * t;
                for (int k = 0; k < 2; ++k) m.tex[k] = p.tex[k] + (q.tex[k] - p.tex[k]) * t;
                out.push_back(m);
            }
        }
        poly.swap(out);
    }
    if (poly.size() < 3) return;
    for (const auto& v : poly)
        if (!(v.pos[3] > 0)) return;

    std::vector<WinVertex> wv;
    wv.reserve(poly.size());
    for (const auto& v : poly) wv.push_back(to_window(v, s));

    // Facing, from the signed area of the whole polygon.
    float area = 0;
    for (size_t i = 0; i < wv.size(); ++i) {
        const auto& p = wv[i];
        const auto& q = wv[(i + 1) % wv.size()];
        area += p.x * q.y - q.x * p.y;
    }
    if (area == 0) return;
    const bool front = (area > 0) == s.front_ccw;
    if (s.cull) {
        if (s.cull_face == kFrontAndBack) return;
        if (s.cull_face == kFront && front) return;
        if (s.cull_face == kBack && !front) return;
    }
    for (size_t i = 1; i + 1 < wv.size(); ++i) fill(wv[0], wv[i], wv[i + 1], s, s.flat ? flat : nullptr);
}

void Raster::fill(const WinVertex& a, const WinVertex& b, const WinVertex& c, const RasterState& s,
                  const float* flat) {
    const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (area == 0) return;
    int bx0, by0, bx1, by1;
    bounds(s, bx0, by0, bx1, by1);
    const int x0 = std::max(bx0, int(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min(bx1, int(std::ceil(std::max({a.x, b.x, c.x}))) + 1);
    const int y0 = std::max(by0, int(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min(by1, int(std::ceil(std::max({a.y, b.y, c.y}))) + 1);
    const float inv_area = 1.0f / area;

    // Top-left rule: a pixel center exactly on an edge belongs to the
    // triangle only for edges on its top or left.
    auto is_top_left = [&](const WinVertex& p, const WinVertex& q) {
        float ex = q.x - p.x, ey = q.y - p.y;
        if (area < 0) ex = -ex, ey = -ey;
        const bool top_left = (ey > 0) || (ey == 0 && ex < 0);
        return top_left;
    };
    const bool tl0 = is_top_left(b, c), tl1 = is_top_left(c, a), tl2 = is_top_left(a, b);

    for (int y = y0; y < y1; ++y) {
        const float py = float(y) + 0.5f;
        for (int x = x0; x < x1; ++x) {
            const float px = float(x) + 0.5f;
            float w0 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * inv_area;
            float w1 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * inv_area;
            float w2 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * inv_area;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if ((w0 == 0 && !tl0) || (w1 == 0 && !tl1) || (w2 == 0 && !tl2)) continue;
            const float z = w0 * a.z + w1 * b.z + w2 * c.z;
            const float iw = w0 * a.inv_w + w1 * b.inv_w + w2 * c.inv_w;
            const float wc = 1.0f / iw;
            float color[4], tex[2];
            if (flat) {
                for (int i = 0; i < 4; ++i) color[i] = flat[i];
            } else {
                for (int i = 0; i < 4; ++i) color[i] = (w0 * a.color[i] + w1 * b.color[i] + w2 * c.color[i]) * wc;
            }
            for (int i = 0; i < 2; ++i) tex[i] = (w0 * a.tex[i] + w1 * b.tex[i] + w2 * c.tex[i]) * wc;
            shade(x, y, z, color, tex, s);
        }
    }
}

void Raster::shade(int x, int y, float z, const float color_in[4], const float tex[2], const RasterState& s) {
    const size_t idx = size_t(h_ - 1 - y) * size_t(w_) + size_t(x);
    float c[4];
    for (int i = 0; i < 4; ++i) c[i] = std::clamp(color_in[i], 0.0f, 1.0f);

    if (s.texture) {
        float t[4];
        sample_texture(*s.texture, tex[0], tex[1], t);
        switch (s.tex_env) {
            case kReplace: for (int i = 0; i < 4; ++i) c[i] = t[i]; break;
            case kDecal:
                for (int i = 0; i < 3; ++i) c[i] = c[i] * (1 - t[3]) + t[i] * t[3];
                break;
            case kBlend:
                for (int i = 0; i < 3; ++i) c[i] = c[i] * (1 - t[i]) + s.tex_env_color[i] * t[i];
                c[3] *= t[3];
                break;
            case kAdd:
                for (int i = 0; i < 3; ++i) c[i] = std::min(1.0f, c[i] + t[i]);
                c[3] *= t[3];
                break;
            case kModulate:
            default: for (int i = 0; i < 4; ++i) c[i] *= t[i]; break;
        }
    }

    if (s.alpha_test && !compare(s.alpha_func, c[3], s.alpha_ref)) return;
    z = std::clamp(z, 0.0f, 1.0f);
    if (s.depth_test) {
        if (!compare(s.depth_func, z, depth_[idx])) return;
        if (s.depth_write) depth_[idx] = z;
    }

    float out[4];
    if (s.blend) {
        float d[4], fs[4], fd[4];
        unpack(color_[idx], d);
        blend_factor(s.blend_src, c, d, fs);
        blend_factor(s.blend_dst, c, d, fd);
        for (int i = 0; i < 4; ++i) out[i] = c[i] * fs[i] + d[i] * fd[i];
    } else {
        for (int i = 0; i < 4; ++i) out[i] = c[i];
    }
    uint32_t p = pack(out), keep = 0;
    for (int i = 0; i < 4; ++i)
        if (!s.color_write[i]) keep |= 0xffu << (8 * i);
    color_[idx] = (color_[idx] & keep) | (p & ~keep);
}

void Raster::line(const Vertex& a, const Vertex& b, const RasterState& s) {
    // A quad of the line's width, in clip space, perpendicular to the line on screen.
    if (!(a.pos[3] > 0) || !(b.pos[3] > 0)) return;
    const float ax = a.pos[0] / a.pos[3] * float(s.vp_w), ay = a.pos[1] / a.pos[3] * float(s.vp_h);
    const float bx = b.pos[0] / b.pos[3] * float(s.vp_w), by = b.pos[1] / b.pos[3] * float(s.vp_h);
    float dx = bx - ax, dy = by - ay;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len == 0) return;
    // Offset in NDC for half the width, perpendicular to the line.
    const float half = std::max(s.line_width, 1.0f) * 0.5f;
    const float nx = -dy / len * half * 2 / float(s.vp_w), ny = dx / len * half * 2 / float(s.vp_h);
    auto offset = [&](const Vertex& v, float sign) {
        Vertex o = v;
        o.pos[0] += sign * nx * v.pos[3];
        o.pos[1] += sign * ny * v.pos[3];
        return o;
    };
    RasterState st = s;
    st.cull = false;
    std::vector<Vertex> poly = {offset(a, 1), offset(a, -1), offset(b, -1), offset(b, 1)};
    draw_clipped(poly, st);
}

void Raster::point(const Vertex& a, const RasterState& s) {
    if (!(a.pos[3] > 0)) return;
    const float hx = std::max(s.point_size, 1.0f) / float(s.vp_w) * a.pos[3];
    const float hy = std::max(s.point_size, 1.0f) / float(s.vp_h) * a.pos[3];
    Vertex v[4] = {a, a, a, a};
    v[0].pos[0] -= hx, v[0].pos[1] -= hy;
    v[1].pos[0] += hx, v[1].pos[1] -= hy;
    v[2].pos[0] += hx, v[2].pos[1] += hy;
    v[3].pos[0] -= hx, v[3].pos[1] += hy;
    RasterState st = s;
    st.cull = false;
    std::vector<Vertex> poly(v, v + 4);
    draw_clipped(poly, st);
}

}  // namespace didj::emu::hle
