// ARM926EJ-S user-mode CPU on Unicorn. Supervisor calls arrive through
// on_interrupt (from inside uc_emu_start); request_stop() ends the current
// run at the next translation-block boundary.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct uc_struct;

namespace didj::emu {

constexpr int kRegSP = 13, kRegLR = 14, kRegPC = 15, kRegCPSR = 16;
constexpr uint32_t kCpsrThumb = 1u << 5;
constexpr uint32_t kCpsrUserMode = 0x10;

// Exception numbers as reported by Unicorn's ARM target.
constexpr uint32_t kExcpSwi = 2;
constexpr uint32_t kExcpBkpt = 7;

struct CpuContext {
    uint32_t r[17] = {};  // r0-r15, cpsr
};

class Cpu {
public:
    Cpu();
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    uc_struct* uc() const { return uc_; }

    uint32_t reg(int r) const;
    void set_reg(int r, uint32_t v);
    void save(CpuContext& ctx) const;
    void load(const CpuContext& ctx);

    enum class Exit { Stopped, Fault };
    // Runs from the current PC until request_stop(), a fault, or (if
    // slice_us > 0) the time slice expires.
    Exit run(uint32_t slice_us);
    void request_stop();

    // Called for SVC/BKPT with the exception number; PC already points past
    // the instruction.
    std::function<void(uint32_t intno)> on_interrupt;

    // Description of the last fault (bad access, undefined instruction).
    const std::string& fault() const { return fault_; }

private:
    friend struct CpuHooks;
    void watchdog_loop();

    uc_struct* uc_ = nullptr;
    std::string fault_;
    uint64_t pending_fault_addr_ = 0;
    int pending_fault_type_ = -1;

    // Preemption: a persistent thread stops the CPU when a slice expires.
    std::thread watchdog_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool running_ = false;
    bool quit_ = false;
    bool in_hook_ = false;        // the emulation thread is inside on_interrupt
    bool stop_deferred_ = false;  // a slice expired during the hook
    uint64_t generation_ = 0;
    std::chrono::steady_clock::time_point deadline_;
};

}  // namespace didj::emu
