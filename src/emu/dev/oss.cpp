#include "emu/dev/oss.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "emu/linux_abi.h"
#include "emu/memory.h"

namespace didj::emu {

using namespace lx;

namespace {

constexpr uint32_t AFMT_U8 = 0x08, AFMT_S16_LE = 0x10;

class OssFile : public OpenFile {
public:
    explicit OssFile(std::shared_ptr<AudioOutput> out) : out_(std::move(out)) { out_->configure(rate_, channels_); }

    // Bytes free in the emulated hardware buffer.
    size_t free_bytes() {
        const size_t total = size_t(frag_count_) * frag_size_;
        const size_t used = out_->queued_frames() * frame_bytes();
        return used >= total ? 0 : total - used;
    }
    size_t frame_bytes() const { return size_t(channels_) * (fmt_ == AFMT_U8 ? 1 : 2); }

    int64_t write(const void* buf, size_t len) override {
        // Like the OSS driver: wait for a whole fragment (or the whole
        // request, if smaller) to be free, then take what fits.
        size_t room = free_bytes();
        if (room < std::min<size_t>(len, frag_size_)) return (flags & kO_NONBLOCK) ? -kEAGAIN : kWouldBlock;
        const size_t fb = frame_bytes();
        size_t frames = std::min(len, room) / fb;
        if (!frames) return int64_t(len);  // a partial frame: swallow it
        std::vector<int16_t> pcm(frames * size_t(channels_));
        if (fmt_ == AFMT_U8) {
            const auto* s = static_cast<const uint8_t*>(buf);
            for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t((int(s[i]) - 128) << 8);
        } else {
            std::memcpy(pcm.data(), buf, pcm.size() * 2);
        }
        out_->write(pcm.data(), frames);
        return int64_t(frames * fb);
    }

    int16_t poll_events() override { return free_bytes() >= frag_size_ ? kPOLLOUT : 0; }

    int ioctl(uint32_t cmd, uint32_t arg, GuestMemory& mem) override {
        auto in = [&](uint32_t& v) { return mem.read_u32(arg, v); };
        auto out = [&](uint32_t v) { return mem.write_u32(arg, v) ? 0 : -kEFAULT; };
        uint32_t v = 0;
        const size_t fb = frame_bytes();
        switch (cmd) {
            case 0x5000: case 0x5008: return 0;  // RESET, POST
            case 0x5001:                         // SYNC: wait until played out
                return out_->queued_frames() ? int(kWouldBlock) : 0;
            case 0xc0045002:  // SPEED
                if (!in(v)) return -kEFAULT;
                rate_ = int(std::clamp<uint32_t>(v, 4000, 48000));
                out_->configure(rate_, channels_);
                return out(uint32_t(rate_));
            case 0xc0045003:  // STEREO
                if (!in(v)) return -kEFAULT;
                channels_ = v ? 2 : 1;
                out_->configure(rate_, channels_);
                return out(v ? 1 : 0);
            case 0xc0045006:  // CHANNELS
                if (!in(v)) return -kEFAULT;
                channels_ = v >= 2 ? 2 : 1;
                out_->configure(rate_, channels_);
                return out(uint32_t(channels_));
            case 0xc0045005:  // SETFMT
                if (!in(v)) return -kEFAULT;
                if (v == AFMT_U8 || v == AFMT_S16_LE) fmt_ = v;
                return out(fmt_);
            case 0x8004500b: return out(AFMT_U8 | AFMT_S16_LE);  // GETFMTS
            case 0xc0045004: return out(frag_size_);             // GETBLKSIZE
            case 0xc004500a:                                     // SETFRAGMENT 0xMMMMSSSS
                if (!in(v)) return -kEFAULT;
                frag_size_ = 1u << std::clamp<uint32_t>(v & 0xffff, 7, 16);
                frag_count_ = std::clamp<uint32_t>(v >> 16, 2, 64);
                return 0;
            case 0xc0045009: return out(1);  // SUBDIVIDE
            case 0x8010500c: {               // GETOSPACE: audio_buf_info
                uint32_t room = uint32_t(free_bytes());
                uint32_t info[4] = {room / frag_size_, frag_count_, frag_size_, room};
                return mem.write(arg, info, sizeof info) ? 0 : -kEFAULT;
            }
            case 0x8004500f: return out(0x100 | 0x2000);  // GETCAPS: DSP_CAP_REVISION | DSP_CAP_TRIGGER
            case 0x80045010: return out(2);               // GETTRIGGER: output enabled
            case 0x40045010: return 0;                    // SETTRIGGER
            case 0x500e: flags |= kO_NONBLOCK; return 0;  // NONBLOCK
            case 0x80045017: return out(uint32_t(out_->queued_frames() * fb));  // GETODELAY
            case 0x800c5011: {  // GETOPTR: count_info {bytes, blocks, ptr}
                uint32_t info[3] = {0, 0, 0};
                return mem.write(arg, info, sizeof info) ? 0 : -kEFAULT;
            }
            default: return -kEINVAL;
        }
    }

    int fstat(StatInfo& st) override {
        st = StatInfo();
        st.mode = kS_IFCHR | 0666;
        st.rdev = make_dev(14, 3);
        return 0;
    }

private:
    std::shared_ptr<AudioOutput> out_;
    int rate_ = 8000, channels_ = 1;
    uint32_t fmt_ = AFMT_U8;
    uint32_t frag_size_ = 4096, frag_count_ = 8;
};

}  // namespace

void NullAudioOutput::drain() {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    queued_ = std::max(0.0, queued_ - elapsed * rate_);
}

void NullAudioOutput::configure(int rate, int) {
    std::lock_guard<std::mutex> lock(mu_);
    drain();
    rate_ = rate;
}

void NullAudioOutput::write(const int16_t*, size_t frames) {
    std::lock_guard<std::mutex> lock(mu_);
    drain();
    queued_ += double(frames);
}

size_t NullAudioOutput::queued_frames() {
    std::lock_guard<std::mutex> lock(mu_);
    drain();
    return size_t(queued_);
}

void register_oss_device(Vfs& vfs, std::shared_ptr<AudioOutput> out) {
    vfs.add_device(14, 3, [out](uint32_t flags) {
        auto f = std::make_shared<OssFile>(out);
        f->flags = flags;
        return f;
    });
}

}  // namespace didj::emu
