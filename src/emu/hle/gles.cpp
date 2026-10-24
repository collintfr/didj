#include "emu/hle/gles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "emu/cpu.h"
#include "emu/dev/basic.h"
#include "emu/dev/lf1000_display.h"
#include "emu/memory.h"
#include "emu/process.h"
#include "emu/vfs.h"

namespace didj::emu::hle {

float HleCall::f(int i) const {
    float v;
    std::memcpy(&v, &args[size_t(i)], 4);
    return v;
}

namespace {

// Everything the original libopengles_lite.so exports under these prefixes.
const std::vector<std::string> kExports = {
    "eglBindTexImage", "eglChooseConfig", "eglCopyBuffers", "eglCreateContext", "eglCreatePbufferSurface",
    "eglCreatePixmapSurface", "eglCreateWindowSurface", "eglDestroyContext", "eglDestroySurface",
    "eglGetConfigAttrib", "eglGetConfigs", "eglGetCurrentContext", "eglGetCurrentDisplay",
    "eglGetCurrentSurface", "eglGetDisplay", "eglGetError", "eglGetProcAddress", "eglInitialize",
    "eglMakeCurrent", "eglQueryContext", "eglQueryString", "eglQuerySurface", "eglReleaseTexImage",
    "eglSurfaceAttrib", "eglSwapBuffers", "eglSwapInterval", "eglTerminate", "eglWaitGL", "eglWaitNative",
    "glActiveTexture", "glAlphaFunc", "glAlphaFuncx", "glBindBuffer", "glBindHWBufferOES", "glBindTexture",
    "glBlendFunc", "glBufferData", "glBufferSubData", "glClear", "glClearColor", "glClearColorx",
    "glClearDepthf", "glClearDepthx", "glClearStencil", "glClientActiveTexture", "glClipPlanef",
    "glClipPlanex", "glColor4f", "glColor4ub", "glColor4x", "glColorMask", "glColorPointer",
    "glCompressedTexImage2D", "glCompressedTexSubImage2D", "glCopyTexImage2D", "glCopyTexSubImage2D",
    "glCullFace", "glCurrentPaletteMatrixOES", "glDeleteBuffers", "glDeleteHWBuffersOES", "glDeleteTextures",
    "glDepthFunc", "glDepthMask", "glDepthRangef", "glDepthRangex", "glDisable", "glDisableClientState",
    "glDrawArrays", "glDrawElements", "glDrawTexfOES", "glDrawTexfvOES", "glDrawTexiOES", "glDrawTexivOES",
    "glDrawTexsOES", "glDrawTexsvOES", "glDrawTexxOES", "glDrawTexxvOES", "glEnable", "glEnableClientState",
    "glFinish", "glFlush", "glFogf", "glFogfv", "glFogx", "glFogxv", "glFrontFace", "glFrustumf",
    "glFrustumx", "glGenBuffers", "glGenHWBuffersOES", "glGenTextures", "glGetBooleanv",
    "glGetBufferParameteriv", "glGetClipPlanef", "glGetClipPlanex", "glGetError", "glGetFixedv",
    "glGetFloatv", "glGetHWBufferParameterivOES", "glGetIntegerv", "glGetLightfv", "glGetLightxv",
    "glGetMaterialfv", "glGetMaterialxv", "glGetPointerv", "glGetString", "glGetTexEnvfv", "glGetTexEnviv",
    "glGetTexEnvxv", "glGetTexParameterfv", "glGetTexParameteriv", "glGetTexParameterxv", "glHWBufferDataOES",
    "glHWBufferSubDataOES", "glHint", "glIsBuffer", "glIsEnabled", "glIsHWBufferOES", "glIsTexture",
    "glLightModelf", "glLightModelfv", "glLightModelx", "glLightModelxv", "glLightf", "glLightfv", "glLightx",
    "glLightxv", "glLineWidth", "glLineWidthx", "glLoadIdentity", "glLoadMatrixf", "glLoadMatrixx",
    "glLoadPaletteFromModelViewMatrixOES", "glLogicOp", "glMaterialf", "glMaterialfv", "glMaterialx",
    "glMaterialxv", "glMatrixIndexPointerOES", "glMatrixMode", "glMultMatrixf", "glMultMatrixx",
    "glMultiTexCoord4f", "glMultiTexCoord4x", "glNormal3f", "glNormal3x", "glNormalPointer", "glOrthof",
    "glOrthox", "glPixelStorei", "glPointParameterf", "glPointParameterfv", "glPointParameterx",
    "glPointParameterxv", "glPointSize", "glPointSizePointerOES", "glPointSizex", "glPolygonOffset",
    "glPolygonOffsetx", "glPopMatrix", "glPushMatrix", "glQueryMatrixxOES", "glReadPixels", "glRotatef",
    "glRotatex", "glSampleCoverage", "glSampleCoveragex", "glScalef", "glScalex", "glScissor", "glShadeModel",
    "glStencilFunc", "glStencilMask", "glStencilOp", "glTexColorKeyOES", "glTexCoordPointer", "glTexEnvf",
    "glTexEnvfv", "glTexEnvi", "glTexEnviv", "glTexEnvx", "glTexEnvxv", "glTexImage2D",
    "glTexImage2DFromVideoOES", "glTexParameterf", "glTexParameterfv", "glTexParameteri", "glTexParameteriv",
    "glTexParameterx", "glTexParameterxv", "glTexSubImage2D", "glTranslatef", "glTranslatex",
    "glVertexPointer", "glViewport", "glWeightPointerOES", "gluLookAt", "gluOrtho2D", "gluPerspective",
    "gluPickMatrix", "gluProject", "gluUnProject", "gluUnProject4",
};

// What the original imports from libDisplayMPI, plus usleep for frame pacing.
enum Import { kInitalize, kSwapBufferCallback, kSetDisplayAddress, kWaitForDisplayAddressPatched, kSetWindow,
              kGetWindowSize, kFinalize, kUsleep, kImportCount };
const std::vector<std::string> kImports = {
    "GLESOAL_Initalize", "GLESOAL_SwapBufferCallback", "GLESOAL_SetDisplayAddress",
    "GLESOAL_WaitForDisplayAddressPatched", "GLESOAL_SetWindow", "GLESOAL_GetWindowSize", "GLESOAL_Finalize",
    "usleep"};

// GL enums.
constexpr uint32_t GL_NO_ERROR = 0, GL_INVALID_ENUM = 0x500, GL_INVALID_VALUE = 0x501,
                   GL_INVALID_OPERATION = 0x502, GL_STACK_OVERFLOW = 0x503, GL_STACK_UNDERFLOW = 0x504;
constexpr uint32_t GL_BYTE = 0x1400, GL_UNSIGNED_BYTE = 0x1401, GL_SHORT = 0x1402, GL_UNSIGNED_SHORT = 0x1403,
                   GL_FLOAT = 0x1406, GL_FIXED = 0x140c;
constexpr uint32_t GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701, GL_TEXTURE = 0x1702;
constexpr uint32_t GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY = 0x8075, GL_COLOR_ARRAY = 0x8076,
                   GL_TEXTURE_COORD_ARRAY = 0x8078;
constexpr uint32_t GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr uint32_t GL_TEXTURE_2D = 0x0de1, GL_TEXTURE_CROP_RECT_OES = 0x8b9d;
constexpr uint32_t GL_RGBA = 0x1908, GL_RGB = 0x1907, GL_ALPHA = 0x1906, GL_LUMINANCE = 0x1909,
                   GL_LUMINANCE_ALPHA = 0x190a;
constexpr uint32_t GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034,
                   GL_UNSIGNED_SHORT_5_6_5 = 0x8363;

uint32_t float_bits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}

int32_t to_fixed(float f) { return int32_t(std::lround(double(f) * 65536.0)); }

using Mat = std::array<float, 16>;

Mat identity() {
    Mat m{};
    m[0] = m[5] = m[10] = m[15] = 1;
    return m;
}

Mat multiply(const Mat& a, const Mat& b) {  // column-major a * b
    Mat r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[size_t(k * 4 + row)] * b[size_t(c * 4 + k)];
            r[size_t(c * 4 + row)] = s;
        }
    return r;
}

void transform(const Mat& m, const float in[4], float out[4]) {
    for (int row = 0; row < 4; ++row)
        out[row] = m[size_t(row)] * in[0] + m[size_t(4 + row)] * in[1] + m[size_t(8 + row)] * in[2] +
                   m[size_t(12 + row)] * in[3];
}

Mat translate(float x, float y, float z) {
    Mat m = identity();
    m[12] = x, m[13] = y, m[14] = z;
    return m;
}

Mat scale(float x, float y, float z) {
    Mat m = identity();
    m[0] = x, m[5] = y, m[10] = z;
    return m;
}

Mat rotate(float deg, float x, float y, float z) {
    Mat m = identity();
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len == 0) return m;
    x /= len, y /= len, z /= len;
    const float a = deg * 3.14159265358979f / 180.0f, c = std::cos(a), s = std::sin(a), t = 1 - c;
    m[0] = x * x * t + c, m[4] = x * y * t - z * s, m[8] = x * z * t + y * s;
    m[1] = y * x * t + z * s, m[5] = y * y * t + c, m[9] = y * z * t - x * s;
    m[2] = x * z * t - y * s, m[6] = y * z * t + x * s, m[10] = z * z * t + c;
    return m;
}

Mat ortho(float l, float r, float b, float t, float n, float f) {
    Mat m = identity();
    m[0] = 2 / (r - l), m[5] = 2 / (t - b), m[10] = -2 / (f - n);
    m[12] = -(r + l) / (r - l), m[13] = -(t + b) / (t - b), m[14] = -(f + n) / (f - n);
    return m;
}

Mat frustum(float l, float r, float b, float t, float n, float f) {
    Mat m{};
    m[0] = 2 * n / (r - l), m[5] = 2 * n / (t - b);
    m[8] = (r + l) / (r - l), m[9] = (t + b) / (t - b), m[10] = -(f + n) / (f - n), m[11] = -1;
    m[14] = -2 * f * n / (f - n);
    return m;
}

uint32_t expand_bits(uint32_t v, int bits) {
    v &= (1u << bits) - 1;
    return (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
}

uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }

// One texel from guest pixel data; returns its size in bytes, 0 if unsupported.
int decode_texel(uint32_t format, uint32_t type, const uint8_t* p, uint32_t& out) {
    if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
            case GL_RGBA: out = rgba(p[0], p[1], p[2], p[3]); return 4;
            case GL_RGB: out = rgba(p[0], p[1], p[2], 255); return 3;
            case GL_LUMINANCE: out = rgba(p[0], p[0], p[0], 255); return 1;
            case GL_ALPHA: out = rgba(0, 0, 0, p[0]); return 1;
            case GL_LUMINANCE_ALPHA: out = rgba(p[0], p[0], p[0], p[1]); return 2;
            default: return 0;
        }
    }
    const uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8;
    switch (type) {
        case GL_UNSIGNED_SHORT_5_6_5:
            out = rgba(expand_bits(v >> 11, 5), expand_bits(v >> 5, 6), expand_bits(v, 5), 255);
            return 2;
        case GL_UNSIGNED_SHORT_4_4_4_4:
            out = rgba(expand_bits(v >> 12, 4), expand_bits(v >> 8, 4), expand_bits(v >> 4, 4), expand_bits(v, 4));
            return 2;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            out = rgba(expand_bits(v >> 11, 5), expand_bits(v >> 6, 5), expand_bits(v >> 1, 5), (v & 1) ? 255 : 0);
            return 2;
        default: return 0;
    }
}

}  // namespace

const std::vector<std::string>& GlesHle::export_names() { return kExports; }

GlesHle::GlesHle(Process& proc, Lf1000Display& display) : p_(proc), display_(display) {
    lib_ = build_stub_library("libopengles_lite.so", kExports, kImports);
    for (auto& s : stacks_) s.assign(1, identity());
    register_egl();
    register_gl();
    handlers_.resize(kExports.size());
    for (size_t i = 0; i < kExports.size(); ++i) {
        auto it = by_name_.find(kExports[i]);
        if (it != by_name_.end()) handlers_[i] = it->second;
    }
}

GlesHle::~GlesHle() = default;

void GlesHle::install(Vfs& vfs) {
    std::string image(lib_.image.begin(), lib_.image.end());
    vfs.add_synthetic(kLibraryPath, [image](uint32_t) { return make_memory_file(image, 1); });
}

void GlesHle::def(const char* name, Handler h) { by_name_[name] = std::move(h); }

void GlesHle::on_svc(uint32_t imm) {
    Cpu& cpu = p_.cpu();
    const int tid = p_.current().tid;
    HleCall c;
    if (imm == kHleSvcReturn) {
        auto it = pending_.find(tid);
        if (it == pending_.end()) {
            std::fprintf(stderr, "[gles] stray callback return in thread %d\n", tid);
            cpu.set_reg(12, 0);
            return;
        }
        c = it->second.call;
        pending_.erase(it);
        c.callback_result = cpu.reg(0);
        c.calling = false;
        c.result = 0;
    } else {
        c.index = imm - kHleSvcBase;
        if (c.index >= kExports.size()) {
            cpu.set_reg(0, 0), cpu.set_reg(12, 0);
            return;
        }
        if (!base_) base_ = cpu.reg(kRegPC) - lib_.svc_offset(c.index) - 4;
        for (int i = 0; i < 4; ++i) c.args[size_t(i)] = cpu.reg(i);
        const uint32_t sp = cpu.reg(kRegSP) + 8;  // above the thunk's push {r4, lr}
        for (size_t i = 4; i < c.args.size(); ++i) p_.mem().read_u32(sp + uint32_t(i - 4) * 4, c.args[i]);
        if (trace)
            std::fprintf(stderr, "[gles] %s(%x, %x, %x, %x, %x, %x)\n", kExports[c.index].c_str(), c.args[0],
                         c.args[1], c.args[2], c.args[3], c.args[4], c.args[5]);
    }

    const Handler& h = handlers_[c.index];
    if (h) {
        h(c);
    } else if (reported_.insert(c.index).second) {
        std::fprintf(stderr, "[gles] %s is not implemented\n", kExports[c.index].c_str());
    }

    if (c.calling) {
        HleCall saved = c;
        saved.step = c.step + 1;
        pending_[tid] = Pending{saved};
        for (int i = 0; i < 4; ++i) cpu.set_reg(i, c.call_args[size_t(i)]);
        cpu.set_reg(12, c.call_fn);
    } else {
        cpu.set_reg(0, c.result);
        cpu.set_reg(12, 0);
    }
}

uint32_t GlesHle::scratch(uint32_t offset) {
    if (!scratch_) scratch_ = p_.mem().map_anywhere(0x4000, 0, "[gles-hle]");
    return scratch_ + offset;
}

uint32_t GlesHle::guest_string(const std::string& s) {
    auto it = strings_.find(s);
    if (it != strings_.end()) return it->second;
    uint32_t at = scratch(0x1000);
    for (const auto& kv : strings_) at = std::max(at, kv.second + uint32_t(kv.first.size()) + 1);
    p_.mem().write(at, s.c_str(), s.size() + 1);
    strings_[s] = at;
    return at;
}

void GlesHle::put_u32(uint32_t addr, uint32_t v) {
    if (addr) p_.mem().write_u32(addr, v);
}

uint32_t GlesHle::import(int i) const {
    uint32_t v = 0;
    if (base_) p_.mem().read_u32(base_ + lib_.import_offset + uint32_t(i) * 4, v);
    return v;
}

// --- EGL ---

void GlesHle::init_oal(HleCall& c, int first_step, int& next_step) {
    // Steps first_step and first_step + 1: GLESOAL_Initalize(&meminfo, 0),
    // which has the Display module set up OpenGL (InitOpenGL).
    if (c.step == first_step) {
        if (oal_ready_ || !import(kInitalize)) {
            oal_ready_ = true;
            next_step = first_step + 2;
            return;
        }
        c.call(import(kInitalize), scratch(0x40), 0);
        return;
    }
    if (c.step == first_step + 1) {
        for (uint32_t i = 0; i < meminfo_.size(); ++i) p_.mem().read_u32(scratch(0x40) + i * 4, meminfo_[i]);
        oal_ready_ = true;
        if (trace)
            std::fprintf(stderr, "[gles] GLESOAL_Initalize -> %u, meminfo %x %x %x %x %x %x %x\n", c.callback_result,
                         meminfo_[0], meminfo_[1], meminfo_[2], meminfo_[3], meminfo_[4], meminfo_[5], meminfo_[6]);
        next_step = first_step + 2;
    }
}

void GlesHle::register_egl() {
    constexpr uint32_t kDisplay = 1, kConfig = 1, kSurface = 1, kContext = 1;
    def("eglGetDisplay", [](HleCall& c) { c.ret(kDisplay); });
    def("eglInitialize", [this](HleCall& c) {
        put_u32(c.u(1), 1);
        put_u32(c.u(2), 1);
        c.ret(1);
    });
    def("eglTerminate", [this](HleCall& c) {
        if (c.step == 0 && oal_ready_ && import(kFinalize)) {
            oal_ready_ = false;
            c.call(import(kFinalize));
            return;
        }
        c.ret(1);
    });
    def("eglGetError", [this](HleCall& c) {
        c.ret(egl_error_);
        egl_error_ = 0x3000;
    });
    def("eglQueryString", [this](HleCall& c) {
        switch (c.u(1)) {
            case 0x3053: c.ret(guest_string("LeapFrog")); break;   // EGL_VENDOR
            case 0x3054: c.ret(guest_string("1.1 didj")); break;   // EGL_VERSION
            case 0x3055: c.ret(guest_string("")); break;           // EGL_EXTENSIONS
            default: c.ret(0); break;
        }
    });
    auto configs = [this](HleCall& c, uint32_t out, uint32_t size, uint32_t num) {
        if (out && size) put_u32(out, kConfig);
        put_u32(num, 1);
        c.ret(1);
    };
    def("eglGetConfigs", [configs](HleCall& c) { configs(c, c.u(1), c.u(2), c.u(3)); });
    def("eglChooseConfig", [this, configs](HleCall& c) {
        if (trace) {
            std::string attrs;
            for (uint32_t a = c.u(1), n = 0; a && n < 32; a += 8, ++n) {
                uint32_t k = 0, v = 0;
                p_.mem().read_u32(a, k);
                if (k == 0x3038) break;  // EGL_NONE
                p_.mem().read_u32(a + 4, v);
                char buf[32];
                std::snprintf(buf, sizeof buf, " %x=%x", k, v);
                attrs += buf;
            }
            std::fprintf(stderr, "[gles] eglChooseConfig attribs:%s\n", attrs.c_str());
        }
        configs(c, c.u(2), c.u(3), c.u(4));
    });
    def("eglGetConfigAttrib", [this](HleCall& c) {
        uint32_t v = 0;
        switch (c.u(2)) {
            case 0x3020: v = 16; break;            // EGL_BUFFER_SIZE
            case 0x3021: v = 0; break;             // EGL_ALPHA_SIZE
            case 0x3022: v = 5; break;             // EGL_BLUE_SIZE
            case 0x3023: v = 6; break;             // EGL_GREEN_SIZE
            case 0x3024: v = 5; break;             // EGL_RED_SIZE
            case 0x3025: v = 16; break;            // EGL_DEPTH_SIZE
            case 0x3028: v = kConfig; break;       // EGL_CONFIG_ID
            case 0x3033: v = 4; break;             // EGL_SURFACE_TYPE: EGL_WINDOW_BIT
            case 0x302c: v = win_w_; break;        // EGL_MAX_PBUFFER_WIDTH
            case 0x302a: v = win_h_; break;        // EGL_MAX_PBUFFER_HEIGHT
            case 0x3027: v = 0x3038; break;        // EGL_CONFIG_CAVEAT: EGL_NONE
            default: v = 0; break;
        }
        put_u32(c.u(3), v);
        c.ret(1);
    });
    def("eglCreateContext", [this](HleCall& c) {
        int next = -1;
        init_oal(c, 0, next);
        if (c.calling) return;
        // A new context starts from the default GL state: BLT and the game it
        // launches share this process, one context after the other.
        reset_gl();
        c.ret(kContext);
    });
    def("eglCreateWindowSurface", [this](HleCall& c) {
        // 0-1: init; 2: SetWindow(native window); 3: GetWindowSize(&w, &h); 4: done.
        for (;;) {
            int next = -1;
            if (c.step < 2) {
                init_oal(c, 0, next);
                if (c.calling) return;
                c.step = next;
                continue;
            }
            if (c.step == 2) {
                if (!import(kSetWindow)) {
                    c.step = 4;
                    continue;
                }
                c.call(import(kSetWindow), c.u(2));
                return;
            }
            if (c.step == 3) {
                if (!import(kGetWindowSize)) {
                    c.step = 4;
                    continue;
                }
                put_u32(scratch(0x80), 0);
                put_u32(scratch(0x84), 0);
                c.call(import(kGetWindowSize), scratch(0x80), scratch(0x84));
                return;
            }
            if (import(kGetWindowSize)) {
                uint32_t w = 0, h = 0;
                p_.mem().read_u32(scratch(0x80), w);
                p_.mem().read_u32(scratch(0x84), h);
                if (w && h && w <= 1024 && h <= 1024) win_w_ = w, win_h_ = h;
            }
            raster_.resize(int(win_w_), int(win_h_));
            state_.vp_w = state_.sc_w = int(win_w_);
            state_.vp_h = state_.sc_h = int(win_h_);
            if (trace) std::fprintf(stderr, "[gles] window %ux%u\n", win_w_, win_h_);
            c.ret(kSurface);
            return;
        }
    });
    def("eglCreatePbufferSurface", [](HleCall& c) { c.ret(2); });
    def("eglCreatePixmapSurface", [](HleCall& c) { c.ret(3); });
    def("eglDestroySurface", [](HleCall& c) { c.ret(1); });
    def("eglDestroyContext", [](HleCall& c) { c.ret(1); });
    def("eglMakeCurrent", [](HleCall& c) { c.ret(1); });
    def("eglGetCurrentContext", [](HleCall& c) { c.ret(kContext); });
    def("eglGetCurrentDisplay", [](HleCall& c) { c.ret(kDisplay); });
    def("eglGetCurrentSurface", [](HleCall& c) { c.ret(kSurface); });
    def("eglQuerySurface", [this](HleCall& c) {
        uint32_t v = 0;
        if (c.u(2) == 0x3057) v = win_w_;       // EGL_WIDTH
        else if (c.u(2) == 0x3056) v = win_h_;  // EGL_HEIGHT
        put_u32(c.u(3), v);
        c.ret(1);
    });
    def("eglQueryContext", [this](HleCall& c) {
        put_u32(c.u(3), c.u(2) == 0x3028 ? kConfig : 0);
        c.ret(1);
    });
    def("eglSurfaceAttrib", [](HleCall& c) { c.ret(1); });
    def("eglSwapInterval", [](HleCall& c) { c.ret(1); });
    def("eglWaitGL", [](HleCall& c) { c.ret(1); });
    def("eglWaitNative", [](HleCall& c) { c.ret(1); });
    def("eglBindTexImage", [](HleCall& c) { c.ret(1); });
    def("eglReleaseTexImage", [](HleCall& c) { c.ret(1); });
    def("eglCopyBuffers", [](HleCall& c) { c.ret(1); });
    def("eglGetProcAddress", [](HleCall& c) { c.ret(0); });
    def("eglSwapBuffers", [this](HleCall& c) { swap(c); });
}

void GlesHle::present() {
    // The 3D layer: the one showing our frame buffers, else one marked 3D.
    // Its format and stride are what the Display module set up for OpenGL;
    // before that, RGB565 in 4096-byte lines as libDisplay configures it.
    uint32_t format = 0x4432, hstride = 2, vstride = 4096, address = 0;
    {
        std::lock_guard<std::mutex> lock(display_.mutex());
        int pick = -1;
        for (int i = 0; i < Lf1000Display::kLayers; ++i) {
            const auto& l = display_.layer(i);
            if (frame_phys_[0] && (l.address == frame_phys_[0] || l.address == frame_phys_[1])) pick = i;
        }
        for (int i = 0; i < Lf1000Display::kLayers && pick < 0; ++i)
            if (display_.layer(i).is3d) pick = i;
        if (pick >= 0) {
            const auto& l = display_.layer(pick);
            format = l.format, hstride = l.hstride, vstride = l.vstride, address = l.address;
        }
    }
    if (!frame_phys_[0]) {
        // Two frame buffers in the 2D heap the Display module gave
        // GLESOAL_Initalize, where the original allocated them; otherwise the
        // layer's own memory.
        // The top of the 2D heap is used: the video overlay starts at its bottom.
        // Room for the 4096-byte lines of block memory, whatever the stride now.
        const uint32_t size = 4096 * win_h_;
        const uint32_t heap_end = meminfo_[5] + meminfo_[6] * 0x100000;  // the size is in MB
        if (meminfo_[5] && meminfo_[6] && display_.vram_ptr(heap_end - 2 * size, 2 * size)) {
            frame_phys_[0] = heap_end - 2 * size;
            frame_phys_[1] = heap_end - size;
        } else {
            frame_phys_[0] = frame_phys_[1] = address;
        }
        if (trace) std::fprintf(stderr, "[gles] frame buffers at %x and %x\n", frame_phys_[0], frame_phys_[1]);
    }
    const uint32_t w = std::min<uint32_t>(uint32_t(raster_.width()), Lf1000Display::kWidth);
    const uint32_t h = std::min<uint32_t>(uint32_t(raster_.height()), Lf1000Display::kHeight);
    std::vector<uint32_t> pic(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy(&pic[size_t(y) * w], raster_.pixels() + size_t(y) * size_t(raster_.width()), w * 4);
    if (!display_.write_picture(frame_phys_[back_], format, hstride, vstride, pic.data(), w, h) &&
        reported_.insert(0x10000).second)
        std::fprintf(stderr, "[gles] cannot present to %x (format %x, stride %u/%u)\n", frame_phys_[back_], format,
                     hstride, vstride);
    ++frames_;
}

void GlesHle::swap(HleCall& c) {
    // 0: present, SetDisplayAddress(back buffer); 1: WaitForDisplayAddressPatched;
    // 2: SwapBufferCallback (enables the 3D layer on the first frame); 3: pace; 4: done.
    for (;;) {
        switch (c.step) {
            case 0:
                present();
                c.saved[0] = frame_phys_[back_];
                back_ ^= 1;
                if (import(kSetDisplayAddress)) {
                    c.call(import(kSetDisplayAddress), c.saved[0]);
                    return;
                }
                c.step = 1;
                continue;
            case 1:
                if (import(kWaitForDisplayAddressPatched)) {
                    c.call(import(kWaitForDisplayAddressPatched));
                    return;
                }
                c.step = 2;
                continue;
            case 2:
                if (import(kSwapBufferCallback)) {
                    c.call(import(kSwapBufferCallback));
                    return;
                }
                c.step = 3;
                continue;
            case 3: {
                // About 60 frames a second, like the LCD's refresh.
                using namespace std::chrono;
                const auto now = steady_clock::now();
                const auto frame = microseconds(16667);
                const auto due = last_swap_ + frame;
                last_swap_ = now < due && now + frame > due ? due : now;
                if (now < due && import(kUsleep)) {
                    c.call(import(kUsleep), uint32_t(duration_cast<microseconds>(due - now).count()));
                    return;
                }
                c.step = 4;
                continue;
            }
            default: c.ret(1); return;
        }
    }
}

// --- GL ---

void GlesHle::reset_gl() {
    error_ = GL_NO_ERROR;
    matrix_mode_ = GL_MODELVIEW;
    for (auto& st : stacks_) st.assign(1, identity());
    vertex_ = color_ = texcoord_ = normal_ = Array();
    for (int i = 0; i < 4; ++i) cur_color_[i] = 1;
    cur_tex_[0] = cur_tex_[1] = cur_tex_[2] = 0, cur_tex_[3] = 1;
    buffers_.clear();
    next_buffer_ = 1, array_buffer_ = element_buffer_ = 0;
    textures_.clear();
    crop_.clear();
    next_texture_ = 1, texture_ = 0;
    texture_2d_ = false;
    unpack_alignment_ = 4;
    state_ = RasterState();
    state_.vp_w = state_.sc_w = int(win_w_);
    state_.vp_h = state_.sc_h = int(win_h_);
    for (float& v : clear_color_) v = 0;
    clear_depth_ = 1;
}

void GlesHle::set_error(uint32_t e) {
    if (!error_) error_ = e;
}

GlesHle::Mat& GlesHle::top() {
    int i = matrix_mode_ == GL_PROJECTION ? 1 : matrix_mode_ == GL_TEXTURE ? 2 : 0;
    return stacks_[i].back();
}

void GlesHle::mult(const Mat& m) { top() = multiply(top(), m); }

Texture* GlesHle::bound_texture() { return &textures_[texture_]; }

bool GlesHle::read_source(uint32_t buffer, uint32_t offset, void* out, uint32_t len) {
    if (buffer) {
        auto it = buffers_.find(buffer);
        if (it == buffers_.end() || uint64_t(offset) + len > it->second.size()) return false;
        std::memcpy(out, it->second.data() + offset, len);
        return true;
    }
    return p_.mem().read(offset, out, len);
}

bool GlesHle::fetch(const Array& a, uint32_t index, int max, float* out, bool normalize) {
    uint32_t elem = 0;
    switch (a.type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: elem = 1; break;
        case GL_SHORT: case GL_UNSIGNED_SHORT: elem = 2; break;
        case GL_FLOAT: case GL_FIXED: elem = 4; break;
        default: return false;
    }
    const int n = std::min(a.size, max);
    const uint32_t stride = a.stride ? a.stride : elem * uint32_t(a.size);
    uint8_t raw[16];
    if (!read_source(a.buffer, a.pointer + index * stride, raw, elem * uint32_t(n))) return false;
    const uint8_t* p = raw;
    for (int i = 0; i < n; ++i, p += elem) {
        switch (a.type) {
            case GL_BYTE: out[i] = normalize ? float(int8_t(*p)) / 127.0f : float(int8_t(*p)); break;
            case GL_UNSIGNED_BYTE: out[i] = normalize ? float(*p) / 255.0f : float(*p); break;
            case GL_SHORT: {
                int16_t v;
                std::memcpy(&v, p, 2);
                out[i] = normalize ? float(v) / 32767.0f : float(v);
                break;
            }
            case GL_UNSIGNED_SHORT: {
                uint16_t v;
                std::memcpy(&v, p, 2);
                out[i] = normalize ? float(v) / 65535.0f : float(v);
                break;
            }
            case GL_FLOAT: std::memcpy(&out[i], p, 4); break;
            case GL_FIXED: {
                int32_t v;
                std::memcpy(&v, p, 4);
                out[i] = float(v) / 65536.0f;
                break;
            }
        }
    }
    return true;
}

RasterState GlesHle::raster_state() {
    RasterState s = state_;
    s.texture = nullptr;
    if (texture_2d_) {
        auto it = textures_.find(texture_);
        if (it != textures_.end() && it->second.width > 0) s.texture = &it->second;
    }
    return s;
}

void GlesHle::draw(uint32_t mode, uint32_t count, const std::function<uint32_t(uint32_t)>& index) {
    if (!vertex_.enabled || !count) return;
    const Mat mvp = multiply(stacks_[1].back(), stacks_[0].back());
    const Mat& texm = stacks_[2].back();
    const RasterState s = raster_state();
    std::vector<Vertex> v(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t idx = index(i);
        float pos[4] = {0, 0, 0, 1};
        fetch(vertex_, idx, 4, pos, false);
        transform(mvp, pos, v[i].pos);

        float col[4] = {cur_color_[0], cur_color_[1], cur_color_[2], cur_color_[3]};
        if (color_.enabled) fetch(color_, idx, 4, col, true);
        std::memcpy(v[i].color, col, sizeof col);
        float tc[4] = {cur_tex_[0], cur_tex_[1], cur_tex_[2], cur_tex_[3]};
        if (texcoord_.enabled) {
            tc[2] = 0, tc[3] = 1;
            fetch(texcoord_, idx, 4, tc, false);
        }
        float tt[4];
        transform(texm, tc, tt);
        const float q = tt[3] != 0 ? tt[3] : 1;
        v[i].tex[0] = tt[0] / q;
        v[i].tex[1] = tt[1] / q;
    }
    switch (mode) {
        case 0:  // GL_POINTS
            for (const auto& p : v) raster_.point(p, s);
            break;
        case 1:  // GL_LINES
            for (uint32_t i = 0; i + 1 < count; i += 2) raster_.line(v[i], v[i + 1], s);
            break;
        case 2:  // GL_LINE_LOOP
        case 3:  // GL_LINE_STRIP
            for (uint32_t i = 0; i + 1 < count; ++i) raster_.line(v[i], v[i + 1], s);
            if (mode == 2 && count > 2) raster_.line(v[count - 1], v[0], s);
            break;
        case 4:  // GL_TRIANGLES
            for (uint32_t i = 0; i + 2 < count; i += 3) raster_.triangle(v[i], v[i + 1], v[i + 2], s);
            break;
        case 5:  // GL_TRIANGLE_STRIP
            for (uint32_t i = 0; i + 2 < count; ++i) {
                if (i & 1) raster_.triangle(v[i + 1], v[i], v[i + 2], s);
                else raster_.triangle(v[i], v[i + 1], v[i + 2], s);
            }
            break;
        case 6:  // GL_TRIANGLE_FAN
            for (uint32_t i = 1; i + 1 < count; ++i) raster_.triangle(v[0], v[i], v[i + 1], s);
            break;
        default: set_error(GL_INVALID_ENUM); break;
    }
}

void GlesHle::tex_image(uint32_t level, uint32_t internal, int w, int h, uint32_t format, uint32_t type,
                        uint32_t pixels, bool sub, int xoff, int yoff) {
    (void)internal;
    if (level != 0) return;  // mipmaps are not kept; level 0 is always sampled
    if (w < 0 || h < 0 || w > 2048 || h > 2048) return set_error(GL_INVALID_VALUE);
    Texture& t = *bound_texture();
    if (!sub) {
        t.width = w, t.height = h;
        t.texels.assign(size_t(w) * size_t(h), 0xff000000);
        xoff = yoff = 0;
    }
    if (!pixels || !w || !h) return;
    uint32_t probe;
    uint8_t zero[4] = {0, 0, 0, 0};
    const int bpp = decode_texel(format, type, zero, probe);
    if (!bpp) {
        if (reported_.insert(0x20000 + format * 16 + (type & 15)).second)
            std::fprintf(stderr, "[gles] texture format %x type %x is not supported\n", format, type);
        return set_error(GL_INVALID_ENUM);
    }
    uint32_t row = uint32_t(w * bpp);
    row = (row + unpack_alignment_ - 1) / unpack_alignment_ * unpack_alignment_;
    std::vector<uint8_t> data(size_t(row) * size_t(h - 1) + size_t(w * bpp));
    if (!p_.mem().read(pixels, data.data(), data.size())) return set_error(GL_INVALID_VALUE);
    const uint8_t* src = data.data();
    for (int y = 0; y < h; ++y) {
        const int ty = yoff + y;
        if (ty < 0 || ty >= t.height) continue;
        const uint8_t* p = src + size_t(y) * row;
        for (int x = 0; x < w; ++x, p += bpp) {
            const int tx = xoff + x;
            if (tx < 0 || tx >= t.width) continue;
            decode_texel(format, type, p, t.texels[size_t(ty) * size_t(t.width) + size_t(tx)]);
        }
    }
}

void GlesHle::compressed_tex_image(uint32_t level, uint32_t internal, int w, int h, uint32_t size, uint32_t data) {
    // OES_compressed_paletted_texture: a palette, then 4- or 8-bit indices.
    // A level <= 0 is the base level (negative: that many mipmap levels follow).
    if (int32_t(level) > 0) return;
    if (internal < 0x8b90 || internal > 0x8b99) {
        if (reported_.insert(0x30000 + internal).second)
            std::fprintf(stderr, "[gles] compressed texture format %x is not supported\n", internal);
        return set_error(GL_INVALID_ENUM);
    }
    const bool four = internal <= 0x8b94;
    const uint32_t entries = four ? 16 : 256;
    static const struct { uint32_t format, type; int bytes; } kPalette[5] = {
        {GL_RGB, GL_UNSIGNED_BYTE, 3}, {GL_RGBA, GL_UNSIGNED_BYTE, 4}, {GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2},
        {GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2}, {GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2}};
    const auto& pf = kPalette[(internal - 0x8b90) % 5];
    const uint32_t pal_bytes = entries * uint32_t(pf.bytes);
    const uint32_t idx_bytes = four ? (uint32_t(w) * uint32_t(h) + 1) / 2 : uint32_t(w) * uint32_t(h);
    if (w <= 0 || h <= 0 || size < pal_bytes + idx_bytes) return set_error(GL_INVALID_VALUE);
    std::vector<uint8_t> bytes(pal_bytes + idx_bytes);
    if (!p_.mem().read(data, bytes.data(), bytes.size())) return set_error(GL_INVALID_VALUE);
    const uint8_t* src = bytes.data();
    uint32_t pal[256];
    for (uint32_t i = 0; i < entries; ++i) decode_texel(pf.format, pf.type, src + i * uint32_t(pf.bytes), pal[i]);
    Texture& t = *bound_texture();
    t.width = w, t.height = h;
    t.texels.resize(size_t(w) * size_t(h));
    const uint8_t* idx = src + pal_bytes;
    for (size_t i = 0; i < t.texels.size(); ++i) {
        uint32_t k = four ? ((idx[i / 2] >> ((i & 1) ? 0 : 4)) & 15) : idx[i];
        t.texels[i] = pal[k];
    }
}

void GlesHle::tex_parameter(uint32_t pname, float v) {
    Texture& t = *bound_texture();
    const uint32_t e = uint32_t(v);
    switch (pname) {
        case 0x2801: t.min_filter = e; break;  // GL_TEXTURE_MIN_FILTER
        case 0x2800: t.mag_filter = e; break;  // GL_TEXTURE_MAG_FILTER
        case 0x2802: t.wrap_s = e; break;      // GL_TEXTURE_WRAP_S
        case 0x2803: t.wrap_t = e; break;      // GL_TEXTURE_WRAP_T
        case 0x8191: break;                    // GL_GENERATE_MIPMAP
        default: break;
    }
}

void GlesHle::enable(uint32_t cap, bool on) {
    switch (cap) {
        case GL_TEXTURE_2D: texture_2d_ = on; break;
        case 0x0be2: state_.blend = on; break;        // GL_BLEND
        case 0x0bc0: state_.alpha_test = on; break;   // GL_ALPHA_TEST
        case 0x0b71: state_.depth_test = on; break;   // GL_DEPTH_TEST
        case 0x0b44: state_.cull = on; break;         // GL_CULL_FACE
        case 0x0c11: state_.scissor = on; break;      // GL_SCISSOR_TEST
        case 0x0bd0: case 0x0b10: case 0x0b20: case 0x809d: case 0x0ba1: case 0x803a: case 0x8037:
            break;  // dither, point/line smooth, multisample, normalize, rescale normal, polygon offset
        case 0x0b50:  // GL_LIGHTING
        case 0x0b60:  // GL_FOG
        default:
            if (on && reported_.insert(0x40000 + cap).second)
                std::fprintf(stderr, "[gles] glEnable(%x) is not supported\n", cap);
            break;
    }
}

void GlesHle::get_integers(uint32_t pname, uint32_t out) {
    std::vector<int32_t> v;
    switch (pname) {
        case 0x0d33: v = {1024}; break;  // GL_MAX_TEXTURE_SIZE
        case 0x0ba2: v = {state_.vp_x, state_.vp_y, state_.vp_w, state_.vp_h}; break;  // GL_VIEWPORT
        case 0x0c10: v = {state_.sc_x, state_.sc_y, state_.sc_w, state_.sc_h}; break;  // GL_SCISSOR_BOX
        case 0x0ba0: v = {int32_t(matrix_mode_)}; break;
        case 0x0d36: case 0x0d38: case 0x0d39: v = {16}; break;  // max stack depths
        case 0x84e2: v = {1}; break;                              // GL_MAX_TEXTURE_UNITS
        case 0x8069: v = {int32_t(texture_)}; break;              // GL_TEXTURE_BINDING_2D
        case 0x8894: v = {int32_t(array_buffer_)}; break;         // GL_ARRAY_BUFFER_BINDING
        case 0x8895: v = {int32_t(element_buffer_)}; break;
        case 0x0d32: v = {1}; break;                              // GL_MAX_LIGHTS... lights are off
        case 0x0d50: case 0x0d52: case 0x0d53: case 0x0d54: v = {pname == 0x0d53 ? 6 : 5}; break;  // RED/GREEN/BLUE bits
        case 0x0d55: v = {0}; break;                              // GL_ALPHA_BITS
        case 0x0d56: v = {16}; break;                             // GL_DEPTH_BITS
        case 0x0d57: v = {0}; break;                              // GL_STENCIL_BITS
        case 0x86a2: v = {10}; break;                             // GL_NUM_COMPRESSED_TEXTURE_FORMATS
        case 0x86a3: v = {0x8b90, 0x8b91, 0x8b92, 0x8b93, 0x8b94, 0x8b95, 0x8b96, 0x8b97, 0x8b98, 0x8b99}; break;
        default:
            if (reported_.insert(0x50000 + pname).second)
                std::fprintf(stderr, "[gles] glGetIntegerv(%x) is not supported\n", pname);
            v = {0};
            break;
    }
    for (size_t i = 0; i < v.size(); ++i) put_u32(out + uint32_t(i) * 4, uint32_t(v[i]));
}

void GlesHle::draw_tex(float x, float y, float z, float w, float h) {
    // OES_draw_texture: a screen-aligned rectangle showing the crop rectangle.
    auto it = textures_.find(texture_);
    if (!texture_2d_ || it == textures_.end() || it->second.width <= 0) return;
    const Texture& t = it->second;
    std::array<int, 4> crop = {0, 0, t.width, t.height};
    if (auto c = crop_.find(texture_); c != crop_.end()) crop = c->second;
    const float vw = float(state_.vp_w), vh = float(state_.vp_h);
    const float zc = z <= 0 ? -1 : z >= 1 ? 1 : 2 * z - 1;
    auto vert = [&](float px, float py, float s, float tt) {
        Vertex v;
        v.pos[0] = (px - float(state_.vp_x)) / vw * 2 - 1;
        v.pos[1] = (py - float(state_.vp_y)) / vh * 2 - 1;
        v.pos[2] = zc, v.pos[3] = 1;
        std::memcpy(v.color, cur_color_, sizeof v.color);
        v.tex[0] = s / float(t.width);
        v.tex[1] = tt / float(t.height);
        return v;
    };
    const float s0 = float(crop[0]), t0 = float(crop[1]), s1 = float(crop[0] + crop[2]), t1 = float(crop[1] + crop[3]);
    Vertex a = vert(x, y, s0, t0), b = vert(x + w, y, s1, t0), c = vert(x + w, y + h, s1, t1), d = vert(x, y + h, s0, t1);
    RasterState s = raster_state();
    s.cull = false;
    raster_.triangle(a, b, c, s);
    raster_.triangle(a, c, d, s);
}

void GlesHle::register_gl() {
    // Errors, strings, queries.
    def("glGetError", [this](HleCall& c) {
        c.ret(error_);
        error_ = GL_NO_ERROR;
    });
    def("glGetString", [this](HleCall& c) {
        switch (c.u(0)) {
            case 0x1f00: c.ret(guest_string("LeapFrog")); break;
            case 0x1f01: c.ret(guest_string("didj software renderer")); break;
            case 0x1f02: c.ret(guest_string("OpenGL ES-CM 1.1")); break;
            case 0x1f03:
                c.ret(guest_string("GL_OES_compressed_paletted_texture GL_OES_draw_texture "
                                   "GL_OES_fixed_point GL_OES_read_format"));
                break;
            default: set_error(GL_INVALID_ENUM); c.ret(0); break;
        }
    });
    def("glGetIntegerv", [this](HleCall& c) { get_integers(c.u(0), c.u(1)); });
    def("glGetBooleanv", [this](HleCall& c) {
        uint32_t tmp = scratch(0x100);
        get_integers(c.u(0), tmp);
        uint32_t v = 0;
        p_.mem().read_u32(tmp, v);
        uint8_t b = v != 0;
        if (c.u(1)) p_.mem().write(c.u(1), &b, 1);
    });
    auto get_matrix = [this](uint32_t pname, Mat& m) {
        switch (pname) {
            case 0x0ba6: m = stacks_[0].back(); return true;  // GL_MODELVIEW_MATRIX
            case 0x0ba7: m = stacks_[1].back(); return true;  // GL_PROJECTION_MATRIX
            case 0x0ba8: m = stacks_[2].back(); return true;  // GL_TEXTURE_MATRIX
            default: return false;
        }
    };
    def("glGetFloatv", [this, get_matrix](HleCall& c) {
        Mat m;
        if (get_matrix(c.u(0), m)) {
            for (int i = 0; i < 16; ++i) put_u32(c.u(1) + uint32_t(i) * 4, float_bits(m[size_t(i)]));
            return;
        }
        uint32_t tmp = scratch(0x100);
        get_integers(c.u(0), tmp);
        uint32_t v = 0;
        p_.mem().read_u32(tmp, v);
        put_u32(c.u(1), float_bits(float(int32_t(v))));
    });
    def("glGetFixedv", [this, get_matrix](HleCall& c) {
        Mat m;
        if (get_matrix(c.u(0), m)) {
            for (int i = 0; i < 16; ++i) put_u32(c.u(1) + uint32_t(i) * 4, uint32_t(to_fixed(m[size_t(i)])));
            return;
        }
        uint32_t tmp = scratch(0x100);
        get_integers(c.u(0), tmp);
        uint32_t v = 0;
        p_.mem().read_u32(tmp, v);
        put_u32(c.u(1), uint32_t(int32_t(v) * 65536));
    });
    def("glIsEnabled", [this](HleCall& c) {
        switch (c.u(0)) {
            case GL_TEXTURE_2D: c.ret(texture_2d_); break;
            case 0x0be2: c.ret(state_.blend); break;
            case 0x0bc0: c.ret(state_.alpha_test); break;
            case 0x0b71: c.ret(state_.depth_test); break;
            case 0x0b44: c.ret(state_.cull); break;
            case 0x0c11: c.ret(state_.scissor); break;
            case GL_VERTEX_ARRAY: c.ret(vertex_.enabled); break;
            case GL_COLOR_ARRAY: c.ret(color_.enabled); break;
            case GL_TEXTURE_COORD_ARRAY: c.ret(texcoord_.enabled); break;
            default: c.ret(0); break;
        }
    });
    for (const char* n : {"glFlush", "glFinish", "glHint", "glLogicOp", "glStencilFunc", "glStencilMask",
                          "glStencilOp", "glClearStencil", "glSampleCoverage", "glSampleCoveragex",
                          "glPolygonOffset", "glPolygonOffsetx", "glActiveTexture", "glClientActiveTexture"})
        def(n, [](HleCall&) {});

    // Enables.
    def("glEnable", [this](HleCall& c) { enable(c.u(0), true); });
    def("glDisable", [this](HleCall& c) { enable(c.u(0), false); });
    auto client_array = [this](uint32_t cap) -> Array* {
        switch (cap) {
            case GL_VERTEX_ARRAY: return &vertex_;
            case GL_COLOR_ARRAY: return &color_;
            case GL_TEXTURE_COORD_ARRAY: return &texcoord_;
            case GL_NORMAL_ARRAY: return &normal_;
            default: return nullptr;
        }
    };
    def("glEnableClientState", [client_array](HleCall& c) {
        if (Array* a = client_array(c.u(0))) a->enabled = true;
    });
    def("glDisableClientState", [client_array](HleCall& c) {
        if (Array* a = client_array(c.u(0))) a->enabled = false;
    });

    // Per-fragment state.
    def("glBlendFunc", [this](HleCall& c) { state_.blend_src = c.u(0), state_.blend_dst = c.u(1); });
    def("glAlphaFunc", [this](HleCall& c) { state_.alpha_func = c.u(0), state_.alpha_ref = c.f(1); });
    def("glAlphaFuncx", [this](HleCall& c) { state_.alpha_func = c.u(0), state_.alpha_ref = c.x(1); });
    def("glDepthFunc", [this](HleCall& c) { state_.depth_func = c.u(0); });
    def("glDepthMask", [this](HleCall& c) { state_.depth_write = c.u(0) & 0xff; });
    def("glDepthRangef", [this](HleCall& c) { state_.depth_near = c.f(0), state_.depth_far = c.f(1); });
    def("glDepthRangex", [this](HleCall& c) { state_.depth_near = c.x(0), state_.depth_far = c.x(1); });
    def("glColorMask", [this](HleCall& c) {
        for (int i = 0; i < 4; ++i) state_.color_write[i] = c.u(i) & 0xff;
    });
    def("glCullFace", [this](HleCall& c) { state_.cull_face = c.u(0); });
    def("glFrontFace", [this](HleCall& c) { state_.front_ccw = c.u(0) == 0x0901; });  // GL_CCW
    def("glShadeModel", [this](HleCall& c) { state_.flat = c.u(0) == 0x1d00; });     // GL_FLAT
    def("glScissor", [this](HleCall& c) {
        state_.sc_x = c.i(0), state_.sc_y = c.i(1), state_.sc_w = c.i(2), state_.sc_h = c.i(3);
    });
    def("glViewport", [this](HleCall& c) {
        state_.vp_x = c.i(0), state_.vp_y = c.i(1), state_.vp_w = c.i(2), state_.vp_h = c.i(3);
    });
    def("glLineWidth", [this](HleCall& c) { state_.line_width = c.f(0); });
    def("glLineWidthx", [this](HleCall& c) { state_.line_width = c.x(0); });
    def("glPointSize", [this](HleCall& c) { state_.point_size = c.f(0); });
    def("glPointSizex", [this](HleCall& c) { state_.point_size = c.x(0); });

    // Clearing.
    def("glClearColor", [this](HleCall& c) {
        for (int i = 0; i < 4; ++i) clear_color_[i] = c.f(i);
    });
    def("glClearColorx", [this](HleCall& c) {
        for (int i = 0; i < 4; ++i) clear_color_[i] = c.x(i);
    });
    def("glClearDepthf", [this](HleCall& c) { clear_depth_ = c.f(0); });
    def("glClearDepthx", [this](HleCall& c) { clear_depth_ = c.x(0); });
    def("glClear", [this](HleCall& c) {
        raster_.clear(c.u(0) & 0x4000, c.u(0) & 0x100, clear_color_, clear_depth_, state_);
    });

    // Current values.
    def("glColor4f", [this](HleCall& c) {
        for (int i = 0; i < 4; ++i) cur_color_[i] = c.f(i);
    });
    def("glColor4x", [this](HleCall& c) {
        for (int i = 0; i < 4; ++i) cur_color_[i] = c.x(i);
    });
    def("glColor4ub", [this](HleCall& c) {
        for (int i = 0; i < 4; ++i) cur_color_[i] = float(c.u(i) & 0xff) / 255.0f;
    });
    def("glMultiTexCoord4f", [this](HleCall& c) {
        for (int i = 0; i < 4; ++i) cur_tex_[i] = c.f(i + 1);
    });
    def("glMultiTexCoord4x", [this](HleCall& c) {
        for (int i = 0; i < 4; ++i) cur_tex_[i] = c.x(i + 1);
    });
    def("glNormal3f", [](HleCall&) {});
    def("glNormal3x", [](HleCall&) {});

    // Matrices.
    def("glMatrixMode", [this](HleCall& c) { matrix_mode_ = c.u(0); });
    def("glLoadIdentity", [this](HleCall&) { top() = identity(); });
    def("glPushMatrix", [this](HleCall&) {
        auto& st = stacks_[matrix_mode_ == GL_PROJECTION ? 1 : matrix_mode_ == GL_TEXTURE ? 2 : 0];
        if (st.size() >= 32) return set_error(GL_STACK_OVERFLOW);
        st.push_back(st.back());
    });
    def("glPopMatrix", [this](HleCall&) {
        auto& st = stacks_[matrix_mode_ == GL_PROJECTION ? 1 : matrix_mode_ == GL_TEXTURE ? 2 : 0];
        if (st.size() <= 1) return set_error(GL_STACK_UNDERFLOW);
        st.pop_back();
    });
    auto read_matrix = [this](uint32_t addr, bool fixed, Mat& m) {
        for (int i = 0; i < 16; ++i) {
            uint32_t v = 0;
            p_.mem().read_u32(addr + uint32_t(i) * 4, v);
            if (fixed) m[size_t(i)] = float(int32_t(v)) / 65536.0f;
            else std::memcpy(&m[size_t(i)], &v, 4);
        }
    };
    def("glLoadMatrixf", [this, read_matrix](HleCall& c) { read_matrix(c.u(0), false, top()); });
    def("glLoadMatrixx", [this, read_matrix](HleCall& c) { read_matrix(c.u(0), true, top()); });
    def("glMultMatrixf", [this, read_matrix](HleCall& c) {
        Mat m;
        read_matrix(c.u(0), false, m);
        mult(m);
    });
    def("glMultMatrixx", [this, read_matrix](HleCall& c) {
        Mat m;
        read_matrix(c.u(0), true, m);
        mult(m);
    });
    def("glTranslatef", [this](HleCall& c) { mult(translate(c.f(0), c.f(1), c.f(2))); });
    def("glTranslatex", [this](HleCall& c) { mult(translate(c.x(0), c.x(1), c.x(2))); });
    def("glScalef", [this](HleCall& c) { mult(scale(c.f(0), c.f(1), c.f(2))); });
    def("glScalex", [this](HleCall& c) { mult(scale(c.x(0), c.x(1), c.x(2))); });
    def("glRotatef", [this](HleCall& c) { mult(rotate(c.f(0), c.f(1), c.f(2), c.f(3))); });
    def("glRotatex", [this](HleCall& c) { mult(rotate(c.x(0), c.x(1), c.x(2), c.x(3))); });
    def("glOrthof", [this](HleCall& c) { mult(ortho(c.f(0), c.f(1), c.f(2), c.f(3), c.f(4), c.f(5))); });
    def("glOrthox", [this](HleCall& c) { mult(ortho(c.x(0), c.x(1), c.x(2), c.x(3), c.x(4), c.x(5))); });
    def("glFrustumf", [this](HleCall& c) { mult(frustum(c.f(0), c.f(1), c.f(2), c.f(3), c.f(4), c.f(5))); });
    def("glFrustumx", [this](HleCall& c) { mult(frustum(c.x(0), c.x(1), c.x(2), c.x(3), c.x(4), c.x(5))); });

    // Vertex arrays and buffers.
    auto set_array = [this](Array& a, HleCall& c) {
        a.size = c.i(0), a.type = c.u(1), a.stride = c.u(2), a.pointer = c.u(3), a.buffer = array_buffer_;
    };
    def("glVertexPointer", [this, set_array](HleCall& c) { set_array(vertex_, c); });
    def("glColorPointer", [this, set_array](HleCall& c) { set_array(color_, c); });
    def("glTexCoordPointer", [this, set_array](HleCall& c) { set_array(texcoord_, c); });
    def("glNormalPointer", [this](HleCall& c) {
        normal_.size = 3, normal_.type = c.u(0), normal_.stride = c.u(1), normal_.pointer = c.u(2);
        normal_.buffer = array_buffer_;
    });
    def("glGenBuffers", [this](HleCall& c) {
        for (uint32_t i = 0; i < c.u(0); ++i) {
            buffers_[next_buffer_];
            put_u32(c.u(1) + i * 4, next_buffer_++);
        }
    });
    def("glDeleteBuffers", [this](HleCall& c) {
        for (uint32_t i = 0; i < c.u(0); ++i) {
            uint32_t id = 0;
            p_.mem().read_u32(c.u(1) + i * 4, id);
            buffers_.erase(id);
            if (array_buffer_ == id) array_buffer_ = 0;
            if (element_buffer_ == id) element_buffer_ = 0;
        }
    });
    def("glIsBuffer", [this](HleCall& c) { c.ret(buffers_.count(c.u(0)) ? 1 : 0); });
    def("glBindBuffer", [this](HleCall& c) {
        if (c.u(1)) buffers_[c.u(1)];
        if (c.u(0) == GL_ARRAY_BUFFER) array_buffer_ = c.u(1);
        else if (c.u(0) == GL_ELEMENT_ARRAY_BUFFER) element_buffer_ = c.u(1);
        else set_error(GL_INVALID_ENUM);
    });
    auto target_buffer = [this](uint32_t target) -> std::vector<uint8_t>* {
        uint32_t id = target == GL_ARRAY_BUFFER ? array_buffer_ : target == GL_ELEMENT_ARRAY_BUFFER ? element_buffer_ : 0;
        if (!id) return nullptr;
        return &buffers_[id];
    };
    def("glBufferData", [this, target_buffer](HleCall& c) {
        auto* b = target_buffer(c.u(0));
        if (!b) return set_error(GL_INVALID_OPERATION);
        b->assign(c.u(1), 0);
        if (c.u(2) && c.u(1)) p_.mem().read(c.u(2), b->data(), c.u(1));
    });
    def("glBufferSubData", [this, target_buffer](HleCall& c) {
        auto* b = target_buffer(c.u(0));
        if (!b) return set_error(GL_INVALID_OPERATION);
        if (uint64_t(c.u(1)) + c.u(2) > b->size()) return set_error(GL_INVALID_VALUE);
        if (c.u(2)) p_.mem().read(c.u(3), b->data() + c.u(1), c.u(2));
    });
    def("glGetBufferParameteriv", [this, target_buffer](HleCall& c) {
        auto* b = target_buffer(c.u(0));
        put_u32(c.u(2), c.u(1) == 0x8764 && b ? uint32_t(b->size()) : 0);  // GL_BUFFER_SIZE
    });

    // Drawing.
    def("glDrawArrays", [this](HleCall& c) {
        const uint32_t first = c.u(1);
        draw(c.u(0), c.u(2), [first](uint32_t i) { return first + i; });
    });
    def("glDrawElements", [this](HleCall& c) {
        const uint32_t mode = c.u(0), count = c.u(1), type = c.u(2), ptr = c.u(3);
        const uint32_t size = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 0;
        if (!size) return set_error(GL_INVALID_ENUM);
        std::vector<uint8_t> raw(size_t(count) * size);
        if (!read_source(element_buffer_, ptr, raw.data(), uint32_t(raw.size()))) return;
        const uint8_t* p = raw.data();
        std::vector<uint32_t> idx(count);
        for (uint32_t i = 0; i < count; ++i) idx[i] = size == 1 ? p[i] : uint32_t(p[2 * i]) | uint32_t(p[2 * i + 1]) << 8;
        draw(mode, count, [&idx](uint32_t i) { return idx[i]; });
    });
    def("glDrawTexiOES", [this](HleCall& c) {
        draw_tex(float(c.i(0)), float(c.i(1)), float(c.i(2)), float(c.i(3)), float(c.i(4)));
    });
    def("glDrawTexsOES", [this](HleCall& c) {
        draw_tex(float(int16_t(c.u(0))), float(int16_t(c.u(1))), float(int16_t(c.u(2))), float(int16_t(c.u(3))),
                 float(int16_t(c.u(4))));
    });
    def("glDrawTexxOES", [this](HleCall& c) { draw_tex(c.x(0), c.x(1), c.x(2), c.x(3), c.x(4)); });
    def("glDrawTexfOES", [this](HleCall& c) { draw_tex(c.f(0), c.f(1), c.f(2), c.f(3), c.f(4)); });
    auto draw_tex_v = [this](uint32_t addr, int kind) {
        float v[5];
        for (int i = 0; i < 5; ++i) {
            uint32_t w = 0;
            if (kind == 1) {
                uint16_t s = 0;
                p_.mem().read(addr + uint32_t(i) * 2, &s, 2);
                v[i] = float(int16_t(s));
                continue;
            }
            p_.mem().read_u32(addr + uint32_t(i) * 4, w);
            if (kind == 0) v[i] = float(int32_t(w));
            else if (kind == 2) v[i] = float(int32_t(w)) / 65536.0f;
            else std::memcpy(&v[i], &w, 4);
        }
        draw_tex(v[0], v[1], v[2], v[3], v[4]);
    };
    def("glDrawTexivOES", [draw_tex_v](HleCall& c) { draw_tex_v(c.u(0), 0); });
    def("glDrawTexsvOES", [draw_tex_v](HleCall& c) { draw_tex_v(c.u(0), 1); });
    def("glDrawTexxvOES", [draw_tex_v](HleCall& c) { draw_tex_v(c.u(0), 2); });
    def("glDrawTexfvOES", [draw_tex_v](HleCall& c) { draw_tex_v(c.u(0), 3); });

    // Textures.
    def("glGenTextures", [this](HleCall& c) {
        for (uint32_t i = 0; i < c.u(0); ++i) {
            while (textures_.count(next_texture_)) ++next_texture_;
            textures_[next_texture_];
            put_u32(c.u(1) + i * 4, next_texture_++);
        }
    });
    def("glDeleteTextures", [this](HleCall& c) {
        for (uint32_t i = 0; i < c.u(0); ++i) {
            uint32_t id = 0;
            p_.mem().read_u32(c.u(1) + i * 4, id);
            if (!id) continue;
            textures_.erase(id);
            crop_.erase(id);
            if (texture_ == id) texture_ = 0;
        }
    });
    def("glIsTexture", [this](HleCall& c) { c.ret(c.u(0) && textures_.count(c.u(0)) ? 1 : 0); });
    def("glBindTexture", [this](HleCall& c) {
        texture_ = c.u(1);
        textures_[texture_];
    });
    def("glPixelStorei", [this](HleCall& c) {
        if (c.u(0) == 0x0cf5 && (c.u(1) == 1 || c.u(1) == 2 || c.u(1) == 4 || c.u(1) == 8))  // GL_UNPACK_ALIGNMENT
            unpack_alignment_ = c.u(1);
    });
    def("glTexImage2D", [this](HleCall& c) {
        tex_image(c.u(1), c.u(2), c.i(3), c.i(4), c.u(6), c.u(7), c.u(8), false, 0, 0);
    });
    def("glTexSubImage2D", [this](HleCall& c) {
        tex_image(c.u(1), 0, c.i(4), c.i(5), c.u(6), c.u(7), c.u(8), true, c.i(2), c.i(3));
    });
    def("glCompressedTexImage2D", [this](HleCall& c) {
        compressed_tex_image(c.u(1), c.u(2), c.i(3), c.i(4), c.u(6), c.u(7));
    });
    def("glTexParameteri", [this](HleCall& c) { tex_parameter(c.u(1), float(c.u(2))); });
    def("glTexParameterf", [this](HleCall& c) { tex_parameter(c.u(1), c.f(2)); });
    def("glTexParameterx", [this](HleCall& c) { tex_parameter(c.u(1), float(c.u(2))); });  // enums are not scaled
    auto tex_parameter_v = [this](HleCall& c, int kind) {
        if (c.u(1) == GL_TEXTURE_CROP_RECT_OES) {
            std::array<int, 4> r{};
            for (int i = 0; i < 4; ++i) {
                uint32_t v = 0;
                p_.mem().read_u32(c.u(2) + uint32_t(i) * 4, v);
                float fv;
                std::memcpy(&fv, &v, 4);
                r[size_t(i)] = kind == 0 ? int32_t(v) : kind == 1 ? int(fv) : int32_t(v) / 65536;
            }
            crop_[texture_] = r;
            return;
        }
        uint32_t v = 0;
        p_.mem().read_u32(c.u(2), v);
        float fv;
        std::memcpy(&fv, &v, 4);
        tex_parameter(c.u(1), kind == 1 ? fv : float(v));
    };
    def("glTexParameteriv", [tex_parameter_v](HleCall& c) { tex_parameter_v(c, 0); });
    def("glTexParameterfv", [tex_parameter_v](HleCall& c) { tex_parameter_v(c, 1); });
    def("glTexParameterxv", [tex_parameter_v](HleCall& c) { tex_parameter_v(c, 2); });
    auto tex_env = [this](uint32_t pname, uint32_t value, bool is_float) {
        if (pname == 0x2200) {  // GL_TEXTURE_ENV_MODE
            float fv;
            std::memcpy(&fv, &value, 4);
            state_.tex_env = is_float ? uint32_t(fv) : value;
        }
    };
    def("glTexEnvi", [tex_env](HleCall& c) { tex_env(c.u(1), c.u(2), false); });
    def("glTexEnvx", [tex_env](HleCall& c) { tex_env(c.u(1), c.u(2), false); });
    def("glTexEnvf", [tex_env](HleCall& c) { tex_env(c.u(1), c.u(2), true); });
    auto tex_env_v = [this, tex_env](HleCall& c, int kind) {
        if (c.u(1) == 0x2201) {  // GL_TEXTURE_ENV_COLOR
            for (int i = 0; i < 4; ++i) {
                uint32_t v = 0;
                p_.mem().read_u32(c.u(2) + uint32_t(i) * 4, v);
                float fv;
                std::memcpy(&fv, &v, 4);
                state_.tex_env_color[i] = kind == 1 ? fv : kind == 2 ? float(int32_t(v)) / 65536.0f
                                                                     : float(int32_t(v)) / 2147483647.0f;
            }
            return;
        }
        uint32_t v = 0;
        p_.mem().read_u32(c.u(2), v);
        tex_env(c.u(1), v, kind == 1);
    };
    def("glTexEnviv", [tex_env_v](HleCall& c) { tex_env_v(c, 0); });
    def("glTexEnvfv", [tex_env_v](HleCall& c) { tex_env_v(c, 1); });
    def("glTexEnvxv", [tex_env_v](HleCall& c) { tex_env_v(c, 2); });

    def("glReadPixels", [this](HleCall& c) {
        // GL_RGBA/GL_UNSIGNED_BYTE only; rows bottom-up as GL defines them.
        const int x = c.i(0), y = c.i(1), w = c.i(2), h = c.i(3);
        if (c.u(4) != GL_RGBA || c.u(5) != GL_UNSIGNED_BYTE) return set_error(GL_INVALID_OPERATION);
        for (int row = 0; row < h; ++row)
            for (int col = 0; col < w; ++col) {
                const int px = x + col, py = y + row;
                uint32_t v = 0;
                if (px >= 0 && py >= 0 && px < raster_.width() && py < raster_.height())
                    v = raster_.pixel(px, raster_.height() - 1 - py);
                put_u32(c.u(6) + uint32_t(row * w + col) * 4, v);
            }
    });
}

}  // namespace didj::emu::hle
