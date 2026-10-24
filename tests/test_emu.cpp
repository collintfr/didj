// Tests for the CPU and guest memory layer, using hand-assembled ARM code.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <unicorn/unicorn.h>

#include "elf/dynamic.h"
#include "emu/cpu.h"
#include "emu/hle/gles.h"
#include "emu/hle/raster.h"
#include "emu/hle/stub_elf.h"
#include "emu/memory.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

using namespace didj::emu;

constexpr uint32_t kCode = 0x10000;

void put_code(GuestMemory& mem, uint32_t addr, const std::vector<uint32_t>& words) {
    mem.write(addr, words.data(), words.size() * 4);
}

void test_svc() {
    Cpu cpu;
    GuestMemory mem(cpu.uc());
    CHECK(mem.map_fixed(kCode, kPageSize, "code"));
    put_code(mem, kCode, {
                             0xe3a00005,  // mov r0, #5
                             0xe3a07004,  // mov r7, #4
                             0xef000000,  // svc #0
                             0xe2800001,  // add r0, r0, #1
                             0xef000000,  // svc #0
                         });
    std::vector<uint32_t> pcs, r7s;
    cpu.on_interrupt = [&](uint32_t intno) {
        CHECK(intno == kExcpSwi);
        pcs.push_back(cpu.reg(kRegPC));
        r7s.push_back(cpu.reg(7));
        if (pcs.size() == 1) cpu.set_reg(0, 41);  // syscall result
        else cpu.request_stop();
    };
    cpu.set_reg(kRegPC, kCode);
    CHECK(cpu.run(0) == Cpu::Exit::Stopped);
    CHECK(pcs.size() == 2);
    CHECK(pcs.size() == 2 && pcs[0] == kCode + 12 && pcs[1] == kCode + 20);  // PC is past the svc
    CHECK(cpu.reg(0) == 42);
    CHECK(cpu.reg(kRegPC) == kCode + 20);
    CHECK((cpu.reg(kRegCPSR) & 0x1f) == kCpsrUserMode);

    // Resuming continues after the stop point; save/load round-trips.
    CpuContext ctx;
    cpu.save(ctx);
    cpu.set_reg(0, 0);
    cpu.load(ctx);
    CHECK(cpu.reg(0) == 42 && cpu.reg(kRegPC) == kCode + 20);
}

void test_thumb() {
    Cpu cpu;
    GuestMemory mem(cpu.uc());
    CHECK(mem.map_fixed(kCode, kPageSize, "code"));
    const uint16_t thumb[] = {0x2007, 0xdf00, 0xe7fe};  // movs r0, #7; svc 0; b .
    mem.write(kCode, thumb, sizeof thumb);
    bool hit = false;
    cpu.on_interrupt = [&](uint32_t) {
        hit = true;
        CHECK(cpu.reg(kRegPC) == kCode + 4);
        cpu.request_stop();
    };
    cpu.set_reg(kRegCPSR, kCpsrUserMode | kCpsrThumb);
    cpu.set_reg(kRegPC, kCode);
    CHECK(cpu.run(0) == Cpu::Exit::Stopped);
    CHECK(hit && cpu.reg(0) == 7);
}

void test_preempt_and_fault() {
    Cpu cpu;
    GuestMemory mem(cpu.uc());
    CHECK(mem.map_fixed(kCode, kPageSize, "code"));
    put_code(mem, kCode, {0xeafffffe});  // b .
    cpu.set_reg(kRegPC, kCode);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(cpu.run(20000) == Cpu::Exit::Stopped);  // the 20 ms slice ends the loop
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms >= 15 && ms < 2000);
    CHECK(cpu.reg(kRegPC) == kCode);

    put_code(mem, kCode + 8, {0xe3a02402, 0xe5921000});  // mov r2, #0x02000000; ldr r1, [r2]
    cpu.set_reg(kRegPC, kCode + 8);
    CHECK(cpu.run(0) == Cpu::Exit::Fault);
    CHECK(cpu.fault().find("read from unmapped address 0x02000000") != std::string::npos);
}

// Time slicing must never re-execute instructions: a loop of post-increment
// loads and accumulating stores, stopped every few microseconds, must still
// produce exact results (guards cmake/patch_unicorn.cmake).
void test_preemption_is_exact() {
    Cpu cpu;
    GuestMemory mem(cpu.uc());
    CHECK(mem.map_fixed(kCode, kPageSize, "code"));
    CHECK(mem.map_fixed(0x20000, 0x10000, "data"));
    std::vector<uint32_t> data(0x4000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint32_t(i * 2654435761u);
    mem.write(0x20000, data.data(), data.size() * 4);
    CHECK(mem.map_fixed(0x30000, kPageSize, "scratch"));
    // Four load/accumulate/store groups per block give many exit points.
    put_code(mem, kCode, {
                             0xe3a04803,  //        mov  r4, #0x30000    (scratch)
                             0xe3a09000,  //        mov  r9, #0          (rounds)
                             0xe3a08b02,  //        mov  r8, #0x800      (round limit)
                             0xe3a00000,  // outer: mov  r0, #0          (sum)
                             0xe3a01802,  //        mov  r1, #0x20000    (ptr)
                             0xe3a02901,  //        mov  r2, #0x4000     (count)
                             0xe4913004,  // loop:  ldr  r3, [r1], #4
                             0xe0800003,  //        add  r0, r0, r3
                             0xe5840000,  //        str  r0, [r4]
                             0xe4913004,  //        ldr  r3, [r1], #4
                             0xe0800003,  //        add  r0, r0, r3
                             0xe5840000,  //        str  r0, [r4]
                             0xe4913004,  //        ldr  r3, [r1], #4
                             0xe0800003,  //        add  r0, r0, r3
                             0xe5840000,  //        str  r0, [r4]
                             0xe4913004,  //        ldr  r3, [r1], #4
                             0xe0800003,  //        add  r0, r0, r3
                             0xe5840000,  //        str  r0, [r4]
                             0xe2522004,  //        subs r2, r2, #4
                             0x1afffff1,  //        bne  loop
                             0xe2899001,  //        add  r9, r9, #1
                             0xe1590008,  //        cmp  r9, r8
                             0x1affffeb,  //        bne  outer
                             0xef000000,  //        svc  0
                         });
    uint32_t expect = 0;
    for (uint32_t v : data) expect += v;
    bool done = false;
    cpu.on_interrupt = [&](uint32_t) {
        done = true;
        cpu.request_stop();
    };
    cpu.set_reg(kRegPC, kCode);
    // Stop the CPU from another thread as often as possible, like the
    // time-slice watchdog does, and resume each time through save/load.
    std::atomic<bool> hammer{true};
    std::thread stopper([&] {
        while (hammer) {
            cpu.request_stop();
            std::this_thread::sleep_for(std::chrono::microseconds(5));
        }
    });
    int stops = 0;
    while (!done && stops < 10000000) {
        if (cpu.run(0) == Cpu::Exit::Fault) break;
        CpuContext ctx;
        cpu.save(ctx);
        cpu.load(ctx);
        ++stops;
    }
    hammer = false;
    stopper.join();
    CHECK(done);
    CHECK(stops > 10);  // the test only means something if it was preempted
    uint32_t stored = 0;
    mem.read_u32(0x30000, stored);
    CHECK(cpu.reg(0) == expect && stored == expect && cpu.reg(9) == 0x800 && cpu.reg(1) == 0x20000 + 0x10000);
}

void test_memory() {
    Cpu cpu;
    GuestMemory mem(cpu.uc());
    CHECK(mem.map_fixed(0x40000000, 3 * kPageSize, "anon"));
    uint32_t v = 0x11223344;
    CHECK(mem.write(0x40000000 + kPageSize - 2, &v, 4));  // straddles a page, same region
    mem.unmap(0x40000000 + kPageSize, kPageSize);           // punch a hole in the middle
    CHECK(mem.is_mapped(0x40000000, kPageSize));
    CHECK(!mem.is_mapped(0x40000000 + kPageSize, 1));
    CHECK(mem.is_mapped(0x40000000 + 2 * kPageSize, kPageSize));
    CHECK(mem.regions().size() == 2);
    uint16_t lo = 0;
    CHECK(mem.read(0x40000000 + kPageSize - 2, &lo, 2) && lo == 0x3344);
    CHECK(!mem.read(0x40000000 + kPageSize - 2, &v, 4));  // runs into the hole

    // The CPU sees the surviving pieces too.
    uint32_t word = 0xdeadbeef;
    CHECK(mem.write(0x40000000 + 2 * kPageSize, &word, 4));
    uint32_t back = 0;
    CHECK(uc_mem_read(cpu.uc(), 0x40000000 + 2 * kPageSize, &back, 4) == UC_ERR_OK && back == 0xdeadbeef);

    // The hole is found by the allocator, then the region after it.
    CHECK(mem.find_free(kPageSize, 0x40000000) == 0x40000000 + kPageSize);
    CHECK(mem.find_free(2 * kPageSize, 0x40000000) == 0x40000000 + 3 * kPageSize);
    uint32_t a = mem.map_anywhere(5000, 0, "anon2");
    CHECK(a == 0x40000000 + 3 * kPageSize);
    CHECK(mem.ptr(a, 8192) != nullptr && mem.ptr(a, 8193) == nullptr);

    std::string s;
    const char hello[] = "hello";
    mem.write(a, hello, sizeof hello);
    CHECK(mem.read_string(a, s) && s == "hello");
}

void test_stub_library() {
    using namespace didj::emu::hle;
    auto lib = build_stub_library("libtest.so", {"first", "second"}, {"callback"});
    std::string err;
    auto info = didj::elf::parse_dynamic(lib.image, &err);
    CHECK(info.has_value());
    if (!info) return;
    CHECK(info->soname == "libtest.so");
    CHECK(info->needed == std::vector<std::string>{"libc.so.0"});
    CHECK(info->symbols.size() == 3);
    if (info->symbols.size() == 3) {
        CHECK(info->symbols[0].name == "callback" && !info->symbols[0].defined && info->symbols[0].weak);
        CHECK(info->symbols[2].name == "second" && info->symbols[2].defined &&
              info->symbols[2].value == lib.svc_offset(1) - 4);
    }

    // The real library has every export the guest uses; make sure they fit.
    auto gles = build_stub_library("libopengles_lite.so", GlesHle::export_names(), {"a"});
    auto gles_info = didj::elf::parse_dynamic(gles.image, &err);
    CHECK(gles_info && gles_info->symbols.size() == GlesHle::export_names().size() + 1);

    // Run "second" as the dynamic linker would leave it: the import table
    // holds the callback's address. The host asks for callback(10, 20), which
    // returns r0 + r1, and returns that plus one.
    Cpu cpu;
    GuestMemory mem(cpu.uc());
    constexpr uint32_t kBase = 0x100000, kStack = 0x200000, kCallback = 0x300000;
    CHECK(mem.map_fixed(kBase, uint32_t(page_up(lib.image.size())), "lib"));
    CHECK(mem.map_fixed(kStack - kPageSize, kPageSize, "stack"));
    CHECK(mem.map_fixed(kCallback, kPageSize, "code"));
    mem.write(kBase, lib.image.data(), lib.image.size());
    mem.write_u32(kBase + lib.import_offset, kCallback);
    put_code(mem, kCallback, {0xe0800001, 0xe12fff1e});  // add r0, r0, r1; bx lr
    CHECK(mem.map_fixed(kCode, kPageSize, "caller"));
    put_code(mem, kCode, {0xeb000000 | (((kBase + lib.svc_offset(1) - 4) - (kCode + 8)) >> 2 & 0xffffff),  // bl second
                          0xef000000});                                                                    // svc #0: done
    int svcs = 0;
    bool done = false;
    uint32_t sp_at_svc = 0;
    cpu.on_interrupt = [&](uint32_t) {
        uint32_t insn = 0;
        mem.read_u32(cpu.reg(kRegPC) - 4, insn);
        const uint32_t imm = insn & 0xffffff;
        ++svcs;
        if (imm == kHleSvcBase + 1) {
            sp_at_svc = cpu.reg(kRegSP);
            cpu.set_reg(0, 10), cpu.set_reg(1, 20), cpu.set_reg(12, kCallback);
        } else if (imm == kHleSvcReturn) {
            CHECK(cpu.reg(kRegSP) == sp_at_svc);
            cpu.set_reg(0, cpu.reg(0) + 1), cpu.set_reg(12, 0);
        } else {
            done = true;
            cpu.request_stop();
        }
    };
    cpu.set_reg(kRegSP, kStack);
    cpu.set_reg(kRegPC, kCode);
    cpu.run(0);
    CHECK(done && svcs == 3);
    CHECK(cpu.reg(0) == 31);
    CHECK(cpu.reg(kRegSP) == kStack);
    CHECK(sp_at_svc == kStack - 8);
}

void test_raster() {
    using namespace didj::emu::hle;
    Raster r(8, 8);
    RasterState s;
    s.vp_w = s.vp_h = s.sc_w = s.sc_h = 8;
    const float red[4] = {1, 0, 0, 1};
    r.clear(true, true, red, 1, s);
    CHECK(r.pixel(0, 0) == 0xff0000ff && r.pixel(7, 7) == 0xff0000ff);

    auto vert = [](float x, float y, float z, uint32_t rgba) {
        Vertex v{};
        v.pos[0] = x, v.pos[1] = y, v.pos[2] = z, v.pos[3] = 1;
        for (int i = 0; i < 4; ++i) v.color[i] = float((rgba >> (8 * i)) & 0xff) / 255.0f;
        return v;
    };
    // A green triangle over the lower-left half (GL's y is up; row 0 is the top).
    r.triangle(vert(-1, -1, 0, 0xff00ff00), vert(1, -1, 0, 0xff00ff00), vert(-1, 1, 0, 0xff00ff00), s);
    CHECK(r.pixel(0, 7) == 0xff00ff00);  // bottom left
    CHECK(r.pixel(7, 0) == 0xff0000ff);  // top right is still red
    // Back-facing (clockwise) triangles are culled when asked.
    RasterState cull = s;
    cull.cull = true;
    r.triangle(vert(-1, -1, 0, 0xffff0000), vert(-1, 1, 0, 0xffff0000), vert(1, -1, 0, 0xffff0000), cull);
    CHECK(r.pixel(0, 7) == 0xff00ff00);

    // Depth test: a farther blue quad doesn't cover the nearer one.
    RasterState d = s;
    d.depth_test = true;
    r.clear(true, true, red, 1, s);
    auto quad = [&](float z, uint32_t c, const RasterState& st) {
        r.triangle(vert(-1, -1, z, c), vert(1, -1, z, c), vert(1, 1, z, c), st);
        r.triangle(vert(-1, -1, z, c), vert(1, 1, z, c), vert(-1, 1, z, c), st);
    };
    quad(0, 0xff00ff00, d);
    quad(0.5f, 0xffff0000, d);
    CHECK(r.pixel(3, 3) == 0xff00ff00);
    quad(-0.5f, 0xffff0000, d);
    CHECK(r.pixel(3, 3) == 0xffff0000);

    // Texturing (nearest, replace) and blending.
    Texture t;
    t.width = t.height = 2;
    t.texels = {0xff0000ff, 0xff00ff00, 0xffff0000, 0x80ffffff};  // red green / blue half-white
    t.min_filter = t.mag_filter = 0x2600;
    RasterState ts = s;
    ts.texture = &t;
    ts.tex_env = 0x1e01;  // GL_REPLACE
    auto tvert = [&](float x, float y, float u, float v) {
        Vertex o = vert(x, y, 0, 0xffffffff);
        o.tex[0] = u, o.tex[1] = v;
        return o;
    };
    r.triangle(tvert(-1, -1, 0, 0), tvert(1, -1, 1, 0), tvert(1, 1, 1, 1), ts);
    r.triangle(tvert(-1, -1, 0, 0), tvert(1, 1, 1, 1), tvert(-1, 1, 0, 1), ts);
    CHECK(r.pixel(0, 7) == 0xff0000ff);  // texel (0, 0) at the bottom left
    CHECK(r.pixel(7, 7) == 0xff00ff00);
    CHECK(r.pixel(0, 0) == 0xffff0000);
    ts.blend = true;
    ts.blend_src = 0x0302, ts.blend_dst = 0x0303;  // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    r.clear(true, true, red, 1, s);
    r.triangle(tvert(-1, -1, 0, 0), tvert(1, -1, 1, 0), tvert(1, 1, 1, 1), ts);
    r.triangle(tvert(-1, -1, 0, 0), tvert(1, 1, 1, 1), tvert(-1, 1, 0, 1), ts);
    const uint32_t mixed = r.pixel(7, 0);  // half white over red
    CHECK((mixed & 0xff) == 0xff && ((mixed >> 8) & 0xff) >= 0x7e && ((mixed >> 8) & 0xff) <= 0x81);
}

}  // namespace

int main() {
    test_svc();
    test_thumb();
    test_preempt_and_fault();
    // Each run catches a broken build about half the time; repeat.
    for (int i = 0; i < 8; ++i) test_preemption_is_exact();
    test_memory();
    test_stub_library();
    test_raster();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all emulator tests passed\n");
    return 0;
}
