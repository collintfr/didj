// OpenGL ES 1.1 and EGL 1.x for the guest, implemented on the host.
//
// The guest's libopengles_lite.so drives the LF1000's undocumented 3D core
// directly, so it is replaced by a generated library (stub_elf.h) whose
// exports trap into GlesHle. Drawing goes to a software rasterizer, and
// eglSwapBuffers copies the picture into video memory and tells LeapFrog's
// display library (libDisplayMPI) about it through the same GLESOAL_*
// callbacks the original library calls, so the Display module sets up and
// shows the 3D layer as it does on the hardware.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "emu/hle/raster.h"
#include "emu/hle/stub_elf.h"

namespace didj::emu {

class Process;
class Vfs;
class Lf1000Display;

namespace hle {

// One trapped call. Handlers read arguments, then either return a value or
// ask for a guest function to be called (call()); in that case the handler
// runs again with step + 1 and the guest function's result in callback_result.
struct HleCall {
    uint32_t index = 0;
    int step = 0;
    uint32_t callback_result = 0;
    std::array<uint32_t, 12> args{};
    // A handler's working values that must survive a guest call.
    std::array<uint32_t, 4> saved{};

    uint32_t u(int i) const { return args[size_t(i)]; }
    int32_t i(int i) const { return int32_t(args[size_t(i)]); }
    float x(int i) const { return float(int32_t(args[size_t(i)])) / 65536.0f; }
    float f(int i) const;

    void ret(uint32_t v) { result = v; }
    void call(uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
        calling = true;
        call_fn = fn;
        call_args = {a0, a1, a2, a3};
    }

    uint32_t result = 0;
    bool calling = false;
    uint32_t call_fn = 0;
    std::array<uint32_t, 4> call_args{};
};

class GlesHle {
public:
    static constexpr const char* kLibraryPath = "/Didj/Base/Brio/lib/libopengles_lite.so";

    GlesHle(Process& proc, Lf1000Display& display);
    ~GlesHle();

    // Serves the generated library in place of the original.
    void install(Vfs& vfs);
    // Handles an HLE svc executed by the current thread.
    void on_svc(uint32_t imm);

    bool trace = false;  // log every call
    uint64_t frames() const { return frames_; }
    const Raster& raster() const { return raster_; }

    static const std::vector<std::string>& export_names();

private:
    using Handler = std::function<void(HleCall&)>;
    struct Pending {
        HleCall call;
    };
    struct Array {
        bool enabled = false;
        int size = 4;
        uint32_t type = 0x1406;  // GL_FLOAT
        uint32_t stride = 0;
        uint32_t pointer = 0;
        uint32_t buffer = 0;
    };
    using Mat = std::array<float, 16>;

    void register_egl();
    void register_gl();
    void def(const char* name, Handler h);

    // Guest memory helpers.
    uint32_t scratch(uint32_t offset);  // a host-owned guest page
    uint32_t guest_string(const std::string& s);
    void put_u32(uint32_t addr, uint32_t v);
    uint32_t import(int i) const;

    // GL helpers.
    void reset_gl();
    void set_error(uint32_t e);
    Mat& top();
    void mult(const Mat& m);
    bool fetch(const Array& a, uint32_t index, int max, float* out, bool normalize);
    // Copies len bytes from a buffer object (or guest memory when buffer is 0).
    bool read_source(uint32_t buffer, uint32_t offset, void* out, uint32_t len);
    void draw(uint32_t mode, uint32_t count, const std::function<uint32_t(uint32_t)>& index);
    RasterState raster_state();
    Texture* bound_texture();
    void tex_image(uint32_t level, uint32_t internal, int w, int h, uint32_t format, uint32_t type, uint32_t pixels,
                   bool sub, int xoff, int yoff);
    void compressed_tex_image(uint32_t level, uint32_t internal, int w, int h, uint32_t size, uint32_t data);
    void tex_parameter(uint32_t pname, float v);
    void enable(uint32_t cap, bool on);
    void get_integers(uint32_t pname, uint32_t out);
    void draw_tex(float x, float y, float z, float w, float h);

    // EGL helpers.
    void swap(HleCall& c);
    void init_oal(HleCall& c, int first_step, int& next_step);
    void present();

    Process& p_;
    Lf1000Display& display_;
    StubLibrary lib_;
    uint32_t base_ = 0;  // load address of the generated library
    std::vector<Handler> handlers_;
    std::unordered_map<std::string, Handler> by_name_;
    std::set<uint32_t> reported_;
    std::map<int, Pending> pending_;  // by thread id, while a guest call runs

    uint32_t scratch_ = 0;
    std::map<std::string, uint32_t> strings_;

    // EGL / display hand-off.
    bool oal_ready_ = false;
    std::array<uint32_t, 7> meminfo_{};
    uint32_t win_w_ = 320, win_h_ = 240;
    uint32_t frame_phys_[2] = {0, 0};
    int back_ = 0;
    uint64_t frames_ = 0;
    std::chrono::steady_clock::time_point last_swap_{};
    uint32_t egl_error_ = 0x3000;

    // GL state.
    Raster raster_{320, 240};
    uint32_t error_ = 0;
    uint32_t matrix_mode_ = 0x1700;
    std::vector<Mat> stacks_[3];
    Array vertex_, color_, texcoord_, normal_;
    float cur_color_[4] = {1, 1, 1, 1};
    float cur_tex_[4] = {0, 0, 0, 1};
    std::map<uint32_t, std::vector<uint8_t>> buffers_;
    uint32_t next_buffer_ = 1, array_buffer_ = 0, element_buffer_ = 0;
    std::map<uint32_t, Texture> textures_;
    std::map<uint32_t, std::array<int, 4>> crop_;
    uint32_t next_texture_ = 1, texture_ = 0;
    bool texture_2d_ = false;
    uint32_t unpack_alignment_ = 4;
    RasterState state_;
    float clear_color_[4] = {0, 0, 0, 0};
    float clear_depth_ = 1;
};

}  // namespace hle
}  // namespace didj::emu
