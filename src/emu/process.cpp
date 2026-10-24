#include "emu/process.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

#include "emu/dev/basic.h"
#include "emu/linux_abi.h"
#include "emu/loader.h"
#include "emu/mqueue.h"
#include "emu/syscalls.h"

namespace didj::emu {

using namespace lx;

namespace {

// SA_* flags (ARM)
constexpr uint32_t kSA_SIGINFO = 4, kSA_RESTORER = 0x04000000, kSA_NODEFER = 0x40000000,
                   kSA_RESETHAND = 0x80000000;

constexpr uint64_t sigbit(int sig) { return uint64_t(1) << (sig - 1); }

bool default_ignored(int sig) {
    return sig == 17 /*CHLD*/ || sig == 18 /*CONT*/ || sig == 23 /*URG*/ || sig == 28 /*WINCH*/;
}

// Frame layout (arch/arm/kernel/signal.c, 2.6.20): rt frames start with a
// 128-byte siginfo; then the ucontext, whose sigcontext is 21 words at
// offset 20 and whose signal mask is at offset 104.
constexpr uint32_t kSiginfoSize = 128;
constexpr uint32_t kUcMcontext = 20, kUcSigmask = 104, kUcontextSize = 744;
constexpr uint32_t kRetcodeSize = 8;

}  // namespace

Process::Process(Vfs& vfs, ProcessOptions opts)
    : vfs_(vfs), opts_(opts), mem_(cpu_.uc()), boot_(Clock::now() - std::chrono::seconds(5)) {
    sys_ = std::make_unique<Syscalls>(*this);
    cpu_.on_interrupt = [this](uint32_t intno) { on_interrupt(intno); };

    // Kernel user helpers (arch/arm/kernel/entry-armv.S). The ARM926 has no
    // TLS register, so uClibc's NPTL reads the thread pointer from here.
    mem_.map_fixed(kKuserPage, kPageSize, "[kuser]");
    const uint32_t barrier[] = {0xe12fff1e};  // bx lr
    const uint32_t cmpxchg[] = {
        0xe5923000,  // ldr   r3, [r2]
        0xe0533000,  // subs  r3, r3, r0
        0x05821000,  // streq r1, [r2]
        0xe2730000,  // rsbs  r0, r3, #0   (r0 = 0 and C set on success)
        0xe12fff1e,  // bx    lr
    };
    const uint32_t get_tls[] = {
        0xe59f0008,  // ldr r0, [pc, #8]   -> the word at 0xffff0ff0
        0xe12fff1e,  // bx  lr
    };
    mem_.write(kKuserPage + 0xfa0, barrier, sizeof barrier);
    mem_.write(kKuserPage + 0xfc0, cmpxchg, sizeof cmpxchg);
    mem_.write(kKuserPage + 0xfe0, get_tls, sizeof get_tls);
    mem_.write_u32(kKuserPage + 0xffc, 3);  // __kuser_helper_version
}

Process::~Process() = default;

Clock::duration Process::uptime() const { return Clock::now() - boot_; }

bool Process::exec(const std::string& path, const std::vector<std::string>& argv, const std::vector<std::string>& envp,
                   std::string* err) {
    std::vector<uint8_t> exe_file;
    int r = vfs_.read_file(cwd_, path, exe_file);
    if (r < 0) {
        if (err) *err = path + ": cannot read (errno " + std::to_string(-r) + ")";
        return false;
    }
    ElfImage exe, interp;
    if (!load_elf(mem_, exe_file, path, exe, err)) return false;
    bool dynamic = !exe.interpreter.empty();
    if (dynamic) {
        std::vector<uint8_t> interp_file;
        r = vfs_.read_file("/", exe.interpreter, interp_file);
        if (r < 0) {
            if (err) *err = exe.interpreter + ": cannot read (errno " + std::to_string(-r) + ")";
            return false;
        }
        if (!load_elf(mem_, interp_file, exe.interpreter, interp, err)) return false;
    }
    StartInfo start;
    if (!setup_stack(mem_, exe, dynamic ? &interp : nullptr, argv, envp, start, err)) return false;

    brk_start_ = brk_ = brk_mapped_ = uint32_t(page_up(exe.end));
    if (fds_.empty())
        for (int fd = 0; fd < 3; ++fd) fds_[fd] = Fd{make_console(fd), false};
    auto t = std::make_unique<Thread>();
    t->tid = pid_;
    t->ctx.r[kRegSP] = start.sp;
    t->ctx.r[kRegPC] = start.pc & ~1u;
    t->ctx.r[kRegCPSR] = kCpsrUserMode | ((start.pc & 1) ? kCpsrThumb : 0);
    current_ = t.get();
    threads_.push_back(std::move(t));
    next_tid_ = pid_ + 1;
    return true;
}

int Process::alloc_fd(std::shared_ptr<OpenFile> f, int min_fd, bool cloexec) {
    int fd = std::max(min_fd, 0);
    while (fds_.count(fd)) ++fd;
    if (fd >= 1024) return -kEMFILE;
    fds_[fd] = Fd{std::move(f), cloexec};
    return fd;
}

OpenFile* Process::file(int fd) {
    auto it = fds_.find(fd);
    return it == fds_.end() ? nullptr : it->second.file.get();
}

void Process::on_interrupt(uint32_t intno) {
    if (intno == kExcpSwi) {
        sys_->handle();
        return;
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "unexpected exception %u at pc 0x%08x", intno, cpu_.reg(kRegPC));
    std::fprintf(stderr, "[didj] %s\n", buf);
    exit_process(128 + 5);  // SIGTRAP
    cpu_.request_stop();
}

Clock::time_point Process::from_realtime(int64_t ns) {
    using namespace std::chrono;
    int64_t now = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    return Clock::now() + nanoseconds(ns - now);
}

void Process::fire_timers() {
    const auto now = Clock::now();
    for (auto& [id, tm] : timers_) {
        if (!tm.next || *tm.next > now) continue;
        if (tm.interval.count() > 0) {
            // Periodic: count missed periods as overruns, like the kernel.
            auto late = now - *tm.next;
            int64_t missed = late / tm.interval;
            tm.overrun = int(std::min<int64_t>(missed, 0x7fffffff));
            *tm.next += tm.interval * (missed + 1);
        } else {
            tm.next.reset();
        }
        if (tm.notify == 1) continue;  // SIGEV_NONE
        SigInfo info;
        info.code = -2;  // SI_TIMER
        info.value = tm.value;
        info.timer = id;
        raise_signal(tm.signo, tm.notify == 4 ? tm.tid : 0, info);
    }
}

void Process::raise_signal(int sig, int tid, SigInfo info) {
    if (sig <= 0 || sig > 64) return;
    siginfo_[sig] = info;
    if (!tid) {
        process_pending_ |= sigbit(sig);
        return;
    }
    for (auto& t : threads_)
        if (t->tid == tid && t->state != Thread::State::Exited) t->pending |= sigbit(sig);
}

void Process::setup_signal_frame(Thread& t, int sig, const SigAction& act) {
    const bool rt = act.flags & kSA_SIGINFO;
    const uint32_t frame_size = (rt ? kSiginfoSize : 0) + kUcontextSize + kRetcodeSize;
    uint32_t sp = (t.ctx.r[kRegSP] - frame_size) & ~7u;
    uint32_t uc = sp + (rt ? kSiginfoSize : 0);

    std::vector<uint8_t> frame(frame_size, 0);
    auto put = [&](uint32_t off, uint32_t v) { std::memcpy(&frame[off], &v, 4); };
    if (rt) {
        const SigInfo& info = siginfo_[sig];
        put(0, uint32_t(sig));  // si_signo; si_errno stays 0
        put(8, uint32_t(info.code));
        if (info.code == -2) {   // SI_TIMER: si_tid, si_overrun, si_value
            put(12, uint32_t(info.timer));
            put(16, uint32_t(timers_.count(info.timer) ? timers_[info.timer].overrun : 0));
            put(20, info.value);
        } else {                 // SI_USER/SI_QUEUE/SI_MESGQ: si_pid, si_uid, si_value
            put(12, uint32_t(pid_));
            put(20, info.value);
        }
    }
    uint32_t mc = uc - sp + kUcMcontext;
    put(mc + 8, uint32_t(t.sigmask));  // oldmask
    for (int i = 0; i < 16; ++i) put(mc + 12 + uint32_t(i) * 4, t.ctx.r[i]);  // r0..r15
    put(mc + 12 + 16 * 4, t.ctx.r[kRegCPSR]);
    put(uc - sp + kUcSigmask, uint32_t(t.sigmask));
    put(uc - sp + kUcSigmask + 4, uint32_t(t.sigmask >> 32));
    // Return trampoline for handlers without SA_RESTORER: mov r7, #nr; svc 0
    uint32_t ret = frame_size - kRetcodeSize;
    put(ret, rt ? 0xe3a070ad : 0xe3a07077);
    put(ret + 4, 0xef000000);
    mem_.write(sp, frame.data(), frame.size());

    t.ctx.r[0] = uint32_t(sig);
    t.ctx.r[1] = rt ? sp : 0;
    t.ctx.r[2] = rt ? uc : 0;
    t.ctx.r[kRegSP] = sp;
    t.ctx.r[kRegLR] = (act.flags & kSA_RESTORER) ? act.restorer : sp + ret;
    t.ctx.r[kRegPC] = act.handler & ~1u;
    t.ctx.r[kRegCPSR] = (t.ctx.r[kRegCPSR] & ~kCpsrThumb) | ((act.handler & 1) ? kCpsrThumb : 0);

    t.sigmask |= act.mask;
    if (!(act.flags & kSA_NODEFER)) t.sigmask |= sigbit(sig);
    if (act.flags & kSA_RESETHAND) actions_[sig] = SigAction();
}

void Process::restore_signal_frame(Thread& t, bool rt) {
    // On sigreturn the handler has popped back to the frame it was given.
    uint32_t uc = t.ctx.r[kRegSP] + (rt ? kSiginfoSize : 0);
    uint32_t regs[17];
    mem_.read(uc + kUcMcontext + 12, regs, sizeof regs);
    for (int i = 0; i < 17; ++i) t.ctx.r[i] = regs[i];
    t.ctx.r[kRegCPSR] = (t.ctx.r[kRegCPSR] & ~0x1fu) | kCpsrUserMode;
    uint32_t mask[2];
    mem_.read(uc + kUcSigmask, mask, sizeof mask);
    t.sigmask = mask[0] | (uint64_t(mask[1]) << 32);
    t.sigmask &= ~(sigbit(9) | sigbit(19));  // KILL and STOP cannot be blocked
}

bool Process::deliver_signals(Thread& t) {
    uint64_t deliverable = (t.pending | process_pending_) & ~t.sigmask;
    if (!deliverable) return false;
    for (int sig = 1; sig <= 64; ++sig) {
        if (!(deliverable & sigbit(sig))) continue;
        if (t.pending & sigbit(sig)) t.pending &= ~sigbit(sig);
        else process_pending_ &= ~sigbit(sig);
        const SigAction act = actions_[sig];
        if (act.handler == 1 || (act.handler == 0 && default_ignored(sig))) continue;
        if (act.handler == 0) {
            std::fprintf(stderr, "[didj] killed by signal %d\n", sig);
            exit_process(128 + sig);
            return true;
        }
        if (t.state == Thread::State::Blocked) {
            if (!t.interruptible) {
                t.pending |= sigbit(sig);  // deliver once the wait ends
                return false;
            }
            t.ctx.r[0] = uint32_t(-kEINTR);
            t.state = Thread::State::Runnable;
            t.wake = nullptr;
            t.deadline.reset();
            t.futex_addr = 0;
        }
        setup_signal_frame(t, sig, act);
        return true;
    }
    return false;
}

int Process::futex_wake(uint32_t addr, int count, uint32_t requeue_to, int requeue_max) {
    int woken = 0, moved = 0;
    for (auto& t : threads_) {
        if (t->state != Thread::State::Blocked || t->futex_addr != addr || t->futex_woken) continue;
        if (woken < count) {
            t->futex_woken = true;
            ++woken;
        } else if (requeue_to && moved < requeue_max) {
            t->futex_addr = requeue_to;
            ++moved;
        }
    }
    return woken;
}

void Process::exit_thread(Thread& t, int status) {
    t.state = Thread::State::Exited;
    if (t.clear_child_tid) {
        mem_.write_u32(t.clear_child_tid, 0);
        futex_wake(t.clear_child_tid, 1);
    }
    if (t.tid == pid_ && threads_.size() == 1) exit_process(status);
    bool any_alive = false;
    for (auto& o : threads_) any_alive |= o->state != Thread::State::Exited;
    if (!any_alive) exit_process(status);
}

void Process::exit_process(int status) {
    if (exited_) return;
    exited_ = true;
    exit_status_ = status;
}

bool Process::try_wake(Thread& t, Clock::time_point now) {
    Thread* saved = current_;
    current_ = &t;  // wake() may re-run a syscall on behalf of t
    bool woke = false;
    if (deliver_signals(t)) {
        woke = t.state == Thread::State::Runnable;
    } else if (t.wake && t.wake(t)) {
        woke = true;
    } else if (t.deadline && now >= *t.deadline) {
        t.ctx.r[0] = uint32_t(t.timeout_result);
        t.futex_addr = 0;
        woke = true;
    }
    current_ = saved;
    if (woke) {
        t.state = Thread::State::Runnable;
        t.wake = nullptr;
        t.deadline.reset();
    }
    return woke;
}

Thread* Process::pick_next() {
    if (threads_.empty()) return nullptr;
    const auto now = Clock::now();
    for (auto& t : threads_)
        if (t->state == Thread::State::Blocked) try_wake(*t, now);
    if (exited_) return nullptr;

    // Highest priority wins; round-robin among equals, starting after the
    // thread that ran last.
    auto start = threads_.begin();
    for (auto it = threads_.begin(); it != threads_.end(); ++it)
        if (it->get() == current_) {
            start = std::next(it);
            break;
        }
    Thread* best = nullptr;
    for (size_t n = 0; n < threads_.size(); ++n, ++start) {
        if (start == threads_.end()) start = threads_.begin();
        Thread& t = **start;
        if (t.state != Thread::State::Runnable) continue;
        if (!best || t.effective_priority() > best->effective_priority()) best = &t;
    }
    if (best) {
        deliver_signals(*best);
        if (exited_) return nullptr;
    }
    return best;
}

void Process::preempt_if_needed() {
    if (!current_) return;
    const int prio = current_->effective_priority();
    const auto now = Clock::now();
    for (auto& t : threads_) {
        if (t.get() == current_ || t->effective_priority() <= prio) continue;
        if (t->state == Thread::State::Runnable || (t->state == Thread::State::Blocked && try_wake(*t, now))) {
            cpu_.request_stop();
            return;
        }
    }
}

void Process::switch_to(Thread& t) {
    current_ = &t;
    cpu_.load(t.ctx);
    mem_.write_u32(kKuserPage + 0xff0, t.tls);
}

int Process::run() {
    while (!exited_) {
        if (on_idle) on_idle();
        fire_timers();
        // Reap exited threads (the one that ran last stays until replaced).
        threads_.remove_if([&](const std::unique_ptr<Thread>& t) {
            return t->state == Thread::State::Exited && t.get() != current_;
        });

        Thread* t = pick_next();
        if (exited_) break;
        if (!t) {
            std::optional<Clock::time_point> soonest;
            bool any_blocked = false;
            for (auto& o : threads_) {
                if (o->state != Thread::State::Blocked) continue;
                any_blocked = true;
                if (o->deadline && (!soonest || *o->deadline < *soonest)) soonest = o->deadline;
            }
            for (auto& [id, tm] : timers_)
                if (tm.next && (!soonest || *tm.next < *soonest)) soonest = tm.next;
            if (!any_blocked) break;
            // Nothing can run: sleep until the next timeout, polling for
            // wakeups that come from outside (devices, the front end).
            auto until = Clock::now() + std::chrono::milliseconds(2);
            if (soonest && *soonest < until) until = *soonest;
            std::this_thread::sleep_until(until);
            continue;
        }

        // Slice: until the next timeout of a higher-priority thread, capped so
        // equal-priority threads share the CPU and on_idle runs regularly.
        auto slice = std::chrono::microseconds(opts_.slice_us);
        for (auto& o : threads_) {
            if (o.get() == t || o->state != Thread::State::Blocked || !o->deadline) continue;
            if (o->effective_priority() <= t->effective_priority()) continue;
            auto until = std::chrono::duration_cast<std::chrono::microseconds>(*o->deadline - Clock::now());
            slice = std::max(std::chrono::microseconds(100), std::min(slice, until));
        }
        switch_to(*t);
        Cpu::Exit how = cpu_.run(uint32_t(slice.count()));
        cpu_.save(t->ctx);
        if (how == Cpu::Exit::Fault) {
            std::fprintf(stderr, "[didj] thread %d: %s\n", t->tid, cpu_.fault().c_str());
            const auto& r = t->ctx.r;
            for (int i = 0; i < 16; i += 4)
                std::fprintf(stderr, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, r[i], i + 1, r[i + 1],
                             i + 2, r[i + 2], i + 3, r[i + 3]);
            std::fprintf(stderr, "  cpsr %08x\n", r[16]);
            for (const auto& reg : mem_.regions())
                if ((r[kRegPC] >= reg.base && r[kRegPC] - reg.base < reg.size) ||
                    (r[kRegLR] >= reg.base && r[kRegLR] - reg.base < reg.size))
                    std::fprintf(stderr, "  %08x-%08x %s\n", reg.base, reg.base + reg.size, reg.tag.c_str());
            exit_process(128 + 11);  // SIGSEGV
        }
    }
    return exit_status_;
}

}  // namespace didj::emu
