// LF1000 display hardware as the Didj's drivers expose it: the MLC
// (multi-layer controller) with its RGB layers and video overlay, the DPC
// (LCD timing and backlight), GPIO, and /dev/mem access to video memory.
//
// Ioctl numbers were taken from LeapFrog's own command-line tools
// (layer-control, mlc-control, dpc-control, gpio-control) run under the
// emulator. Software allocates surfaces inside each layer's region of video
// memory and points the layer's address register at them; compose() turns
// the layer state into a 320x240 image.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "emu/vfs.h"

namespace didj::emu {

class Lf1000Display {
public:
    static constexpr uint32_t kWidth = 320, kHeight = 240;
    static constexpr int kLayers = 4;  // 0-1 RGB, 2 video overlay, 3 spare
    // Emulated video memory: one window of physical address space, split
    // into a region per layer (each layer reports its region as address/fbsize).
    static constexpr uint32_t kVramBase = 0x02800000;
    static constexpr uint32_t kLayerRegion = 0x00400000;
    static constexpr uint32_t kVramSize = kLayerRegion * kLayers;

    struct Layer {
        bool enabled = false, blend = false, transparent = false, invert = false, invisible = false, is3d = false;
        uint32_t address = 0, hstride = 2, vstride = 640, format = 0x4432, locksize = 8;
        int32_t top = 0, left = 0, right = int32_t(kWidth) - 1, bottom = int32_t(kHeight) - 1;
        int32_t inv_top = 0, inv_left = 0, inv_right = 0, inv_bottom = 0;
        uint32_t alpha = 15, tpcolor = 0, invcolor = 0;
        // Video overlay: Cb and Cr plane addresses, and the scaler's
        // {source width, source height, output width, output height}.
        uint32_t address_u = 0, address_v = 0;
        int32_t scale[4] = {0, 0, 0, 0};
    };

    Lf1000Display();
    void register_devices(Vfs& vfs);

    // Thread-safe snapshot render into XRGB8888 (kWidth * kHeight pixels).
    void compose(uint32_t* out);

    // Host pointer for a physical address in video memory, or nullptr.
    uint8_t* vram_ptr(uint32_t phys, uint32_t len);

    // Writes an RGBA8 picture (R in the low byte, row 0 at the top) into video
    // memory at phys, in a layer pixel format. Returns false if it doesn't fit.
    bool write_picture(uint32_t phys, uint32_t format, uint32_t hstride, uint32_t vstride, const uint32_t* rgba,
                       uint32_t width, uint32_t height);

    std::mutex& mutex() { return mu_; }
    Layer& layer(int i) { return layers_[i]; }

    // Incremented whenever software marks the display dirty (a new frame).
    uint64_t frame_count() const { return frames_; }

private:
    friend class LayerFile;
    friend class MlcFile;
    friend class DpcFile;
    friend class MemFile;

    void compose_yuv(const Layer& l, uint32_t* out);

    std::mutex mu_;
    std::shared_ptr<uint8_t> vram_;
    Layer layers_[kLayers];
    bool mlc_enabled_ = true;  // the kernel enables the MLC at boot
    uint32_t background_ = 0x000000;
    uint32_t priority_ = 0;
    uint64_t frames_ = 0;
    uint32_t brightness_ = 128, contrast_ = 8, backlight_ = 300;
};

}  // namespace didj::emu
