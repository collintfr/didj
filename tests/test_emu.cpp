// Tests for the CPU and guest memory layer, using hand-assembled ARM code.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <unicorn/unicorn.h>

#include "emu/cpu.h"
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

}  // namespace

int main() {
    test_svc();
    test_thumb();
    test_preempt_and_fault();
    test_memory();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all emulator tests passed\n");
    return 0;
}
