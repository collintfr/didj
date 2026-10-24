#include "emu/cpu.h"

#include <cstdio>
#include <stdexcept>
#include <unicorn/unicorn.h>

namespace didj::emu {

namespace {

const int kUcRegs[17] = {
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,  UC_ARM_REG_R3,  UC_ARM_REG_R4,  UC_ARM_REG_R5,
    UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,  UC_ARM_REG_R9,  UC_ARM_REG_R10, UC_ARM_REG_R11,
    UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR,
};

const char* access_name(int type) {
    switch (type) {
        case UC_MEM_READ_UNMAPPED: return "read from unmapped";
        case UC_MEM_WRITE_UNMAPPED: return "write to unmapped";
        case UC_MEM_FETCH_UNMAPPED: return "fetch from unmapped";
        case UC_MEM_READ_PROT: return "read from protected";
        case UC_MEM_WRITE_PROT: return "write to protected";
        case UC_MEM_FETCH_PROT: return "fetch from protected";
        default: return "bad access to";
    }
}

}  // namespace

struct CpuHooks {
    static void intr(uc_engine*, uint32_t intno, void* user) {
        auto* cpu = static_cast<Cpu*>(user);
        if (cpu->on_interrupt) cpu->on_interrupt(intno);
    }
    static bool bad_access(uc_engine*, uc_mem_type type, uint64_t addr, int, int64_t, void* user) {
        auto* cpu = static_cast<Cpu*>(user);
        cpu->pending_fault_addr_ = addr;
        cpu->pending_fault_type_ = int(type);
        return false;  // makes uc_emu_start fail
    }
};

Cpu::Cpu() {
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc_) != UC_ERR_OK) throw std::runtime_error("uc_open failed");
    uc_ctl_set_cpu_model(uc_, UC_CPU_ARM_926);
    uc_hook h;
    uc_hook_add(uc_, &h, UC_HOOK_INTR, reinterpret_cast<void*>(CpuHooks::intr), this, 1, 0);
    uc_hook_add(uc_, &h, UC_HOOK_MEM_INVALID, reinterpret_cast<void*>(CpuHooks::bad_access), this, 1, 0);
    set_reg(kRegCPSR, kCpsrUserMode);
    watchdog_ = std::thread([this] { watchdog_loop(); });
}

Cpu::~Cpu() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    watchdog_.join();
    uc_close(uc_);
}

uint32_t Cpu::reg(int r) const {
    uint32_t v = 0;
    uc_reg_read(uc_, kUcRegs[r], &v);
    return v;
}

void Cpu::set_reg(int r, uint32_t v) {
    // Unicorn takes the Thumb state from bit 0 of a PC write; keep the
    // current state so callers can set PC and CPSR independently.
    if (r == kRegPC) v = (v & ~1u) | ((reg(kRegCPSR) & kCpsrThumb) ? 1 : 0);
    uc_reg_write(uc_, kUcRegs[r], &v);
}

void Cpu::save(CpuContext& ctx) const {
    void* ptrs[17];
    for (int i = 0; i < 17; ++i) ptrs[i] = &ctx.r[i];
    uc_reg_read_batch(uc_, const_cast<int*>(kUcRegs), ptrs, 17);
}

void Cpu::load(const CpuContext& ctx) {
    // CPSR first: writing it can switch register banks. PC last, carrying
    // the Thumb bit (see set_reg).
    set_reg(kRegCPSR, ctx.r[16]);
    void* ptrs[15];
    for (int i = 0; i < 15; ++i) ptrs[i] = const_cast<uint32_t*>(&ctx.r[i]);
    uc_reg_write_batch(uc_, const_cast<int*>(kUcRegs), ptrs, 15);
    set_reg(kRegPC, ctx.r[15]);
}

void Cpu::watchdog_loop() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!quit_) {
        if (!running_ || deadline_ == std::chrono::steady_clock::time_point()) {
            cv_.wait(lock);
            continue;
        }
        uint64_t gen = generation_;
        if (cv_.wait_until(lock, deadline_) == std::cv_status::timeout && running_ && gen == generation_) {
            uc_emu_stop(uc_);
            deadline_ = std::chrono::steady_clock::time_point();
        }
    }
}

void Cpu::request_stop() { uc_emu_stop(uc_); }

Cpu::Exit Cpu::run(uint32_t slice_us) {
    uint32_t pc = reg(kRegPC);
    uint64_t begin = (reg(kRegCPSR) & kCpsrThumb) ? (pc | 1) : pc;
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = true;
        ++generation_;
        deadline_ = slice_us ? std::chrono::steady_clock::now() + std::chrono::microseconds(slice_us)
                             : std::chrono::steady_clock::time_point();
    }
    cv_.notify_all();
    pending_fault_type_ = -1;
    uc_err err = uc_emu_start(uc_, begin, 0, 0, 0);
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
        deadline_ = std::chrono::steady_clock::time_point();
    }
    if (err == UC_ERR_OK) return Exit::Stopped;

    char buf[160];
    uint32_t at = reg(kRegPC);
    if (pending_fault_type_ >= 0) {
        std::snprintf(buf, sizeof buf, "%s address 0x%08llx at pc 0x%08x", access_name(pending_fault_type_),
                      (unsigned long long)pending_fault_addr_, at);
    } else {
        std::snprintf(buf, sizeof buf, "%s at pc 0x%08x", uc_strerror(err), at);
    }
    fault_ = buf;
    return Exit::Fault;
}

}  // namespace didj::emu
