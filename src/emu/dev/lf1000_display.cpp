#include "emu/dev/lf1000_display.h"

#include <cstdio>
#include <cstring>
#include <set>

#include "emu/linux_abi.h"
#include "emu/memory.h"

namespace didj::emu {

using namespace lx;

namespace {

class DeviceFile : public OpenFile {
public:
    DeviceFile(uint32_t major, uint32_t minor) : rdev_(make_dev(major, minor)) {}
    int fstat(StatInfo& st) override {
        st = StatInfo();
        st.mode = kS_IFCHR | 0660;
        st.rdev = rdev_;
        return 0;
    }

protected:
    // Logs each unknown ioctl once, so new requirements show up in the output.
    int unknown(const char* dev, uint32_t cmd) {
        static std::set<std::pair<std::string, uint32_t>> seen;
        if (seen.insert({dev, cmd}).second) std::fprintf(stderr, "[lf1000] %s: unhandled ioctl 0x%08x\n", dev, cmd);
        return 0;
    }

private:
    uint32_t rdev_;
};

// Expands an n-bit channel to 8 bits.
inline uint32_t expand(uint32_t v, int bits) {
    if (bits <= 0) return 0;
    if (bits >= 8) return v & 0xff;
    v &= (1u << bits) - 1;
    uint32_t out = v << (8 - bits);
    for (int shift = bits; shift < 8; shift += bits) out |= (v << (8 - bits)) >> shift;
    return out & 0xff;
}

struct PixelLayout {
    int bpp;                 // bytes per pixel
    int a, r, g, b;          // channel widths, from high bits to low
    bool luminance = false;  // L8/L16 formats
};

// MLC format codes: bit 15 swaps R and B, bit 14 means "no alpha" (X),
// the low 12 bits select the bit layout.
PixelLayout layout_for(uint32_t format, uint32_t hstride) {
    switch (format & 0xfff) {
        case 0x432: return {2, 0, 5, 6, 5};
        case 0x342: return {2, 1, 5, 5, 5};
        case 0x211: return {2, 4, 4, 4, 4};
        case 0x120: return {2, 8, 3, 3, 2};
        case 0x653: return {hstride == 3 ? 3 : 4, hstride == 3 ? 0 : 8, 8, 8, 8};
        case 0x003: return {2, 8, 0, 0, 0, true};
        case 0x554: return {2, 0, 0, 0, 0, true};
        default: return {int(hstride ? hstride : 2), 0, 5, 6, 5};
    }
}

}  // namespace

// --- /dev/layerN ---

class LayerFile : public DeviceFile {
public:
    LayerFile(Lf1000Display& d, int index) : DeviceFile(250, uint32_t(index)), d_(d), i_(index) {}

    // The driver maps video memory at its physical address, like /dev/mem.
    // Bit 29 selects the block (2D, 4096-byte line) view the video overlay
    // uses; it is linear here.
    int mmap(uint64_t offset, uint32_t len, uint8_t** host, std::shared_ptr<void>* owner) override {
        uint8_t* p = d_.vram_ptr(uint32_t(offset) & ~0x20000000u, len);
        if (!p) return -kEINVAL;
        *host = p;
        *owner = nullptr;
        return 0;
    }

    int ioctl(uint32_t cmd, uint32_t arg, GuestMemory& mem) override {
        std::lock_guard<std::mutex> lock(d_.mu_);
        Lf1000Display::Layer& l = d_.layers_[i_];
        const uint32_t base = Lf1000Display::kVramBase + uint32_t(i_) * Lf1000Display::kLayerRegion;
        switch (cmd) {
            case 0x6d08: l.enabled = arg; return 0;
            case 0x6d09: l.address = arg; return 0;
            case 0x6d0a: l.hstride = arg; return 0;
            case 0x6d0b: l.vstride = arg; return 0;
            case 0x6d0c: l.locksize = arg; return 0;
            case 0x6d0f: l.format = arg; return 0;
            case 0x6d10: return int(l.format);
            case 0x6d11: ++d_.frames_; return 0;  // dirty: the hardware latches the new settings
            case 0x6d12: l.is3d = arg; return 0;
            case 0x6d13: l.alpha = arg; return 0;
            case 0x6d14: l.tpcolor = arg; return 0;
            case 0x6d15: l.blend = arg; return 0;
            case 0x6d16: l.transparent = arg; return 0;
            case 0x6d17: l.invert = arg; return 0;
            case 0x6d18: l.invcolor = arg; return 0;
            case 0x6d19: return int(l.address ? l.address : base);
            case 0x6d1a: return int(l.hstride);
            case 0x6d1b: return int(l.vstride);
            case 0x6d1c: return int(l.alpha);
            case 0x6d1d: return int(Lf1000Display::kLayerRegion);  // fbsize
            case 0x6d20: return 0;  // dirty flag: settings already latched
            case 0x6d22: l.invisible = arg; return 0;
            case 0x6d26: l.address_u = arg; return 0;  // overlay Cb plane
            case 0x6d27: l.address_v = arg; return 0;  // overlay Cr plane
            case 0x40046d1e: {                         // overlay scaler
                int32_t p[4];
                if (!mem.read(arg, p, sizeof p)) return -kEFAULT;
                std::memcpy(l.scale, p, sizeof p);
                return 0;
            }
            case 0x40046d0d:    // set position {top, left, right, bottom}
            case 0x40046d24: {  // set invisible area
                int32_t p[4];
                if (!mem.read(arg, p, sizeof p)) return -kEFAULT;
                int32_t* dst = cmd == 0x40046d0d ? &l.top : &l.inv_top;
                dst[0] = p[0];
                dst[1] = p[1];
                dst[2] = p[2];
                dst[3] = p[3];
                return 0;
            }
            case 0x80046d0e:
            case 0x80046d25: {
                const int32_t* src = cmd == 0x80046d0e ? &l.top : &l.inv_top;
                int32_t p[4] = {src[0], src[1], src[2], src[3]};
                return mem.write(arg, p, sizeof p) ? 0 : -kEFAULT;
            }
            default: return unknown("layer", cmd);
        }
    }

private:
    Lf1000Display& d_;
    int i_;
};

// --- /dev/mlc ---

class MlcFile : public DeviceFile {
public:
    explicit MlcFile(Lf1000Display& d) : DeviceFile(251, 0), d_(d) {}
    int ioctl(uint32_t cmd, uint32_t arg, GuestMemory& mem) override {
        std::lock_guard<std::mutex> lock(d_.mu_);
        switch (cmd) {
            case 0x6d00: d_.mlc_enabled_ = arg; return 0;
            case 0x6d01: d_.background_ = arg & 0xffffff; return 0;
            case 0x6d02: return int(d_.background_);
            case 0x6d03: d_.priority_ = arg; return 0;
            case 0x6d04: return int(d_.priority_);
            case 0x6d05: ++d_.frames_; return 0;
            case 0x6d06: return 0;  // dirty flag: always latched
            case 0x80046d07: {      // screen size {width, height}
                uint32_t sz[2] = {Lf1000Display::kWidth, Lf1000Display::kHeight};
                return mem.write(arg, sz, sizeof sz) ? 0 : -kEFAULT;
            }
            default: return unknown("mlc", cmd);
        }
    }

private:
    Lf1000Display& d_;
};

// --- /dev/dpc: LCD timing, contrast, brightness, backlight ---

class DpcFile : public DeviceFile {
public:
    explicit DpcFile(Lf1000Display& d) : DeviceFile(252, 0), d_(d) {}
    int ioctl(uint32_t cmd, uint32_t arg, GuestMemory&) override {
        std::lock_guard<std::mutex> lock(d_.mu_);
        switch (cmd & 0xffff) {
            case 0x6400: case 0x6401: case 0x6402: case 0x6403: case 0x6404: case 0x6405: case 0x6406:
                return 0;  // interrupt, sync timing, clocks, mode, swaprb
            case 0x6407: if (cmd >> 30 == 1) d_.contrast_ = arg; return int(d_.contrast_);
            case 0x6408: if (cmd >> 30 == 1) d_.brightness_ = arg; return int(d_.brightness_);
            case 0x640a: if (cmd >> 30 == 1) d_.backlight_ = arg; return int(d_.backlight_);
            default: return unknown("dpc", cmd);
        }
    }

private:
    Lf1000Display& d_;
};

// --- /dev/gpio: pins read as 0 (no cartridge-detect edge, buttons via evdev) ---

class GpioFile : public DeviceFile {
public:
    GpioFile() : DeviceFile(253, 0) {}
    int ioctl(uint32_t cmd, uint32_t arg, GuestMemory& mem) override {
        switch (cmd) {
            case 0x40046700: case 0x40046701: case 0x40046703: return 0;  // outvalue, outenable, func
            case 0xc0046702:                                              // invalue: {port, pin} -> value
            case 0xc0046704:                                              // getfunc
                return mem.write_u32(arg, 0) ? 0 : -kEFAULT;
            default: return unknown("gpio", cmd);
        }
    }
};

// --- /dev/mem: physical memory; video memory is shared with the compositor ---

class MemFile : public DeviceFile {
public:
    explicit MemFile(Lf1000Display& d) : DeviceFile(1, 1), d_(d) {}
    int mmap(uint64_t offset, uint32_t len, uint8_t** host, std::shared_ptr<void>* owner) override {
        if (uint8_t* p = d_.vram_ptr(uint32_t(offset), len)) {
            *host = p;
            *owner = nullptr;  // video memory lives as long as the display
            return 0;
        }
        // Register blocks and other memory: scratch pages, logged once.
        std::fprintf(stderr, "[lf1000] /dev/mem mmap of 0x%08llx+0x%x outside video memory (scratch)\n",
                     (unsigned long long)offset, len);
        auto block = std::shared_ptr<uint8_t>(new uint8_t[len](), std::default_delete<uint8_t[]>());
        *host = block.get();
        *owner = block;
        return 0;
    }

private:
    Lf1000Display& d_;
};

// --- devices whose interface is not needed yet: accept and log ---

class LoggingDevice : public DeviceFile {
public:
    LoggingDevice(const char* name, uint32_t major, uint32_t minor) : DeviceFile(major, minor), name_(name) {}
    int ioctl(uint32_t cmd, uint32_t, GuestMemory&) override { return unknown(name_, cmd); }
    int mmap(uint64_t offset, uint32_t len, uint8_t** host, std::shared_ptr<void>* owner) override {
        std::fprintf(stderr, "[lf1000] %s mmap 0x%llx+0x%x (scratch)\n", name_, (unsigned long long)offset, len);
        auto block = std::shared_ptr<uint8_t>(new uint8_t[len](), std::default_delete<uint8_t[]>());
        *host = block.get();
        *owner = block;
        return 0;
    }

private:
    const char* name_;
};

Lf1000Display::Lf1000Display()
    : vram_(new uint8_t[kVramSize](), std::default_delete<uint8_t[]>()) {
    for (int i = 0; i < kLayers; ++i) layers_[i].address = kVramBase + uint32_t(i) * kLayerRegion;
}

void Lf1000Display::compose_yuv(const Layer& l, uint32_t* out) {
    // The video overlay: planar YUV 4:2:0 in block memory (4096-byte lines),
    // scaled from scale[0] x scale[1] source pixels to the layer's rectangle.
    const int32_t dst_w = l.right - l.left, dst_h = l.bottom - l.top;
    if (dst_w <= 0 || dst_h <= 0) return;
    const int32_t src_w = l.scale[0] > 0 ? l.scale[0] : dst_w, src_h = l.scale[1] > 0 ? l.scale[1] : dst_h;
    const uint32_t stride = l.vstride ? l.vstride : 4096;
    const uint32_t y_base = l.address & ~0x20000000u, u_base = l.address_u & ~0x20000000u,
                   v_base = l.address_v & ~0x20000000u;
    for (int32_t y = std::max(l.top, 0); y < std::min(l.bottom, int32_t(kHeight)); ++y) {
        const uint32_t sy = uint32_t((y - l.top) * src_h / dst_h);
        const uint8_t* yr = vram_ptr(y_base + sy * stride, uint32_t(src_w));
        const uint8_t* ur = vram_ptr(u_base + sy / 2 * stride, uint32_t(src_w + 1) / 2);
        const uint8_t* vr = vram_ptr(v_base + sy / 2 * stride, uint32_t(src_w + 1) / 2);
        if (!yr || !ur || !vr) continue;
        for (int32_t x = std::max(l.left, 0); x < std::min(l.right, int32_t(kWidth)); ++x) {
            const uint32_t sx = uint32_t((x - l.left) * src_w / dst_w);
            // BT.601, studio range.
            const int c = int(yr[sx]) - 16, d = int(ur[sx / 2]) - 128, e = int(vr[sx / 2]) - 128;
            const int r = std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255);
            const int g = std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255);
            const int b = std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255);
            out[uint32_t(y) * kWidth + uint32_t(x)] = uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
        }
    }
}

uint8_t* Lf1000Display::vram_ptr(uint32_t phys, uint32_t len) {
    if (phys < kVramBase || uint64_t(phys) + len > uint64_t(kVramBase) + kVramSize) return nullptr;
    return vram_.get() + (phys - kVramBase);
}

void Lf1000Display::register_devices(Vfs& vfs) {
    for (int i = 0; i < kLayers; ++i) {
        vfs.add_device(250, uint32_t(i), [this, i](uint32_t flags) {
            auto f = std::make_shared<LayerFile>(*this, i);
            f->flags = flags;
            return f;
        });
    }
    vfs.add_device(251, 0, [this](uint32_t) { return std::make_shared<MlcFile>(*this); });
    vfs.add_device(252, 0, [this](uint32_t) { return std::make_shared<DpcFile>(*this); });
    vfs.add_device(253, 0, [](uint32_t) { return std::make_shared<GpioFile>(); });
    vfs.add_device(1, 1, [this](uint32_t) { return std::make_shared<MemFile>(*this); });
    vfs.add_device(249, 0, [](uint32_t) { return std::make_shared<LoggingDevice>("ga3d", 249, 0); });
    vfs.add_device(248, 0, [](uint32_t) { return std::make_shared<LoggingDevice>("idct", 248, 0); });
}

void Lf1000Display::compose(uint32_t* out) {
    std::lock_guard<std::mutex> lock(mu_);
    const uint32_t bg = background_;
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) out[i] = bg;
    if (!mlc_enabled_) return;

    // Layer 0 is frontmost on the MLC; draw back to front.
    for (int li = kLayers - 1; li >= 0; --li) {
        const Layer& l = layers_[li];
        if (!l.enabled || l.invisible) continue;
        if (li == 2 && (l.format & 0xfff) == 0) {
            compose_yuv(l, out);
            continue;
        }
        PixelLayout px = layout_for(l.format, l.hstride);
        const bool has_alpha = !(l.format & 0x4000) && px.a > 0;
        const bool swap_rb = l.format & 0x8000;
        const uint32_t global_alpha = l.blend ? std::min<uint32_t>(l.alpha, 15) * 17 : 255;
        for (int32_t y = std::max(l.top, 0); y <= std::min(l.bottom, int32_t(kHeight) - 1); ++y) {
            const uint8_t* row = vram_ptr(l.address + uint32_t(y - l.top) * l.vstride, l.vstride);
            if (!row) continue;
            for (int32_t x = std::max(l.left, 0); x <= std::min(l.right, int32_t(kWidth) - 1); ++x) {
                const uint8_t* p = row + uint32_t(x - l.left) * l.hstride;
                if (p + px.bpp > row + l.vstride) break;
                uint32_t v = 0;
                std::memcpy(&v, p, size_t(px.bpp));
                uint32_t a, r, g, b;
                if (px.luminance) {
                    uint32_t lum = px.a ? (v & 0xff) : (v >> 8) & 0xff;
                    a = px.a ? (v >> 8) & 0xff : 255;
                    r = g = b = lum;
                } else {
                    int shift = 0;
                    b = expand(v >> shift, px.b), shift += px.b;
                    g = expand(v >> shift, px.g), shift += px.g;
                    r = expand(v >> shift, px.r), shift += px.r;
                    a = px.a ? expand(v >> shift, px.a) : 255;
                }
                if (swap_rb) std::swap(r, b);
                uint32_t rgb = (r << 16) | (g << 8) | b;
                if (l.transparent && rgb == (l.tpcolor & 0xffffff)) continue;
                if (l.invert && rgb == (l.invcolor & 0xffffff)) rgb ^= 0xffffff;
                uint32_t alpha = (has_alpha ? a : 255) * global_alpha / 255;
                uint32_t& dst = out[uint32_t(y) * kWidth + uint32_t(x)];
                if (alpha >= 255) {
                    dst = rgb;
                } else if (alpha) {
                    uint32_t dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
                    dr = (r * alpha + dr * (255 - alpha)) / 255;
                    dg = (g * alpha + dg * (255 - alpha)) / 255;
                    db = (b * alpha + db * (255 - alpha)) / 255;
                    dst = (dr << 16) | (dg << 8) | db;
                }
            }
        }
    }
}

bool Lf1000Display::write_picture(uint32_t phys, uint32_t format, uint32_t hstride, uint32_t vstride,
                                  const uint32_t* rgba, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mu_);
    PixelLayout px = layout_for(format, hstride);
    if (!hstride) hstride = uint32_t(px.bpp);
    if (uint64_t(width) * hstride > vstride || px.luminance) return false;
    const bool swap_rb = format & 0x8000;
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = vram_ptr(phys + y * vstride, width * hstride);
        if (!row) return false;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t c = rgba[y * width + x];
            uint32_t r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff, a = c >> 24;
            if (swap_rb) std::swap(r, b);
            uint32_t v = 0;
            int shift = 0;
            auto put = [&](uint32_t ch, int bits) {
                if (bits) v |= (ch >> (8 - bits)) << shift;
                shift += bits;
            };
            put(b, px.b);
            put(g, px.g);
            put(r, px.r);
            put(a, px.a);
            std::memcpy(row + x * hstride, &v, size_t(px.bpp));
        }
    }
    return true;
}

}  // namespace didj::emu
