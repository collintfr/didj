#include "emu/dev/input.h"

#include <chrono>
#include <cstring>

#include "emu/linux_abi.h"
#include "emu/memory.h"

namespace didj::emu {

using namespace lx;

namespace {

constexpr uint16_t EV_SYN = 0, EV_KEY = 1, EV_SW = 5;
const char kName[] = "LF1000 Keyboard";

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

uint16_t linux_keycode(DidjButton b) {
    switch (b) {
        case DidjButton::Up: return 103;
        case DidjButton::Down: return 108;
        case DidjButton::Left: return 105;
        case DidjButton::Right: return 106;
        case DidjButton::A: return 30;
        case DidjButton::B: return 48;
        case DidjButton::LeftShoulder: return 38;   // KEY_L
        case DidjButton::RightShoulder: return 19;  // KEY_R
        case DidjButton::Home: return 50;           // KEY_M
        case DidjButton::Hint: return 35;           // KEY_H
        case DidjButton::Pause: return 25;          // KEY_P
        case DidjButton::Brightness: return 45;     // KEY_X
    }
    return 0;
}

class EvdevFile : public OpenFile {
public:
    EvdevFile(InputHub& hub, std::shared_ptr<std::deque<InputHub::Event>> q) : hub_(hub), q_(std::move(q)) {}

    int64_t read(void* buf, size_t len) override {
        std::lock_guard<std::mutex> lock(hub_.mu_);
        if (len < 16) return -kEINVAL;
        if (q_->empty()) return (flags & kO_NONBLOCK) ? -kEAGAIN : kWouldBlock;
        size_t n = 0;
        auto* out = static_cast<uint8_t*>(buf);
        while (!q_->empty() && n + 16 <= len) {
            const auto& e = q_->front();
            int32_t tv[2] = {int32_t(e.time_us / 1000000), int32_t(e.time_us % 1000000)};
            std::memcpy(out + n, tv, 8);
            std::memcpy(out + n + 8, &e.type, 2);
            std::memcpy(out + n + 10, &e.code, 2);
            std::memcpy(out + n + 12, &e.value, 4);
            q_->pop_front();
            n += 16;
        }
        return int64_t(n);
    }

    int16_t poll_events() override {
        std::lock_guard<std::mutex> lock(hub_.mu_);
        return q_->empty() ? 0 : kPOLLIN;
    }

    int ioctl(uint32_t cmd, uint32_t arg, GuestMemory& mem) override {
        const uint32_t type = (cmd >> 8) & 0xff, nr = cmd & 0xff, size = (cmd >> 16) & 0x3fff;
        if (type != 'E') return -kENOTTY;
        std::lock_guard<std::mutex> lock(hub_.mu_);
        auto put_bits = [&](const uint8_t* bits, size_t nbytes) {
            std::vector<uint8_t> out(size, 0);
            std::memcpy(out.data(), bits, std::min<size_t>(nbytes, size));
            return mem.write(arg, out.data(), out.size()) ? int(std::min<size_t>(nbytes, size)) : -kEFAULT;
        };
        switch (nr) {
            case 0x01: return mem.write_u32(arg, 0x010000) ? 0 : -kEFAULT;  // EVIOCGVERSION
            case 0x02: {                                                    // EVIOCGID
                uint16_t id[4] = {0x19 /*BUS_HOST*/, 0x0001, 0x0001, 0x0100};
                return mem.write(arg, id, sizeof id) ? 0 : -kEFAULT;
            }
            case 0x06: {  // EVIOCGNAME(len)
                size_t n = std::min<size_t>(sizeof kName, size);
                return mem.write(arg, kName, n) ? int(n) : -kEFAULT;
            }
            case 0x07: case 0x08: return -kENOENT;  // EVIOCGPHYS, EVIOCGUNIQ
            case 0x18: {                            // EVIOCGKEY
                uint8_t bits[16] = {};
                for (int k = 0; k < 128; ++k)
                    if (hub_.keys_[k]) bits[k / 8] |= uint8_t(1 << (k % 8));
                return put_bits(bits, sizeof bits);
            }
            case 0x1b: {  // EVIOCGSW
                uint8_t bits[4];
                std::memcpy(bits, &hub_.switches_, 4);
                return put_bits(bits, 1);
            }
            case 0x20: {  // EVIOCGBIT(0): supported event types
                uint8_t bits[4] = {uint8_t((1 << EV_SYN) | (1 << EV_KEY) | (1 << EV_SW)), 0, 0, 0};
                return put_bits(bits, 4);
            }
            case 0x20 + EV_KEY: {
                uint8_t bits[16] = {};
                for (int b = 0; b <= int(DidjButton::Brightness); ++b) {
                    uint16_t k = linux_keycode(DidjButton(b));
                    bits[k / 8] |= uint8_t(1 << (k % 8));
                }
                return put_bits(bits, sizeof bits);
            }
            case 0x20 + EV_SW: {
                uint8_t bits[1] = {0x6};
                return put_bits(bits, 1);
            }
            case 0x90: return 0;  // EVIOCGRAB
            default: return -kEINVAL;
        }
    }

    int fstat(StatInfo& st) override {
        st = StatInfo();
        st.mode = kS_IFCHR | 0660;
        st.rdev = make_dev(13, 64);
        return 0;
    }

private:
    InputHub& hub_;
    std::shared_ptr<std::deque<InputHub::Event>> q_;
};

void InputHub::post(uint16_t type, uint16_t code, int32_t value) {
    int64_t t = now_us();
    for (auto it = queues_.begin(); it != queues_.end();) {
        auto q = it->lock();
        if (!q) {
            it = queues_.erase(it);
            continue;
        }
        q->push_back({t, type, code, value});
        q->push_back({t, EV_SYN, 0, 0});
        ++it;
    }
}

void InputHub::press(DidjButton b, bool down) {
    std::lock_guard<std::mutex> lock(mu_);
    uint16_t k = linux_keycode(b);
    if (k < sizeof keys_) keys_[k] = down;
    post(EV_KEY, k, down ? 1 : 0);
}

void InputHub::set_switch(uint16_t code, bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    if (on) switches_ |= 1u << code;
    else switches_ &= ~(1u << code);
    post(EV_SW, code, on ? 1 : 0);
}

std::shared_ptr<OpenFile> InputHub::open(uint32_t flags) {
    std::lock_guard<std::mutex> lock(mu_);
    auto q = std::make_shared<std::deque<Event>>();
    queues_.push_back(q);
    auto f = std::make_shared<EvdevFile>(*this, q);
    f->flags = flags;
    return f;
}

void InputHub::register_device(Vfs& vfs) {
    vfs.add_device(13, 64, [this](uint32_t flags) { return open(flags); });
}

}  // namespace didj::emu
