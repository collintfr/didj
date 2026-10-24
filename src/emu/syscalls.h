// Linux ARM EABI system calls for a Process.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "emu/process.h"

namespace didj::emu {

class Syscalls {
public:
    explicit Syscalls(Process& p) : p_(p) {}

    // Handles the SVC the current thread just executed.
    void handle();

    static const char* name(uint32_t nr);

private:
    using Args = std::array<uint32_t, 7>;
    // Result values that are not syscall return codes.
    static constexpr int64_t kBlocked = INT64_MIN;    // thread waits; r0 set on wake
    static constexpr int64_t kNoResult = INT64_MIN + 1;  // registers already updated

    int64_t dispatch(uint32_t nr, const Args& a);
    // Blocks the current thread and re-runs the syscall when the scheduler
    // polls it, until it stops returning kBlocked.
    int64_t retry_later(uint32_t nr, const Args& a, std::optional<Clock::time_point> deadline,
                        int32_t timeout_result);

    std::string path_arg(uint32_t addr, bool& ok);
    int64_t do_open(uint32_t path, uint32_t flags, uint32_t mode);
    int64_t do_stat(const StatInfo& st, uint32_t out, bool is64);
    int64_t do_getdents(int fd, uint32_t buf, uint32_t len, bool is64);
    int64_t do_mmap(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int fd, uint64_t off);
    int64_t do_clone(const Args& a);
    int64_t do_futex(const Args& a);
    int64_t do_poll(uint32_t nr, const Args& a);
    int64_t do_select(uint32_t nr, const Args& a);
    int64_t do_ioctl(int fd, uint32_t cmd, uint32_t arg);
    int64_t do_fcntl(int fd, uint32_t cmd, uint32_t arg);
    int64_t do_rt_sigaction(const Args& a);
    int64_t do_rt_sigprocmask(const Args& a);
    int64_t do_sleep(uint32_t nr, const Args& a, uint32_t req, bool absolute, int clock);
    bool read_timespec(uint32_t addr, int64_t& ns);
    void write_timespec(uint32_t addr, int64_t ns);
    int64_t clock_ns(int clock);

    Process& p_;
    bool retrying_ = false;
};

}  // namespace didj::emu
