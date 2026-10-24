#include "emu/dev/board.h"

#include <cstring>

#include "emu/linux_abi.h"

namespace didj::emu {

using namespace lx;

namespace {

// A sysfs attribute: reads return the current value, writes replace it.
class SysfsFile : public OpenFile {
public:
    SysfsFile(BoardState& b, std::string key) : board_(b), key_(std::move(key)), snapshot_(board_.get(key_)) {}
    int64_t read(void* buf, size_t len) override {
        if (pos_ >= snapshot_.size()) return 0;
        size_t n = std::min(len, snapshot_.size() - pos_);
        std::memcpy(buf, snapshot_.data() + pos_, n);
        pos_ += n;
        return int64_t(n);
    }
    int64_t write(const void* buf, size_t len) override {
        board_.set(key_, std::string(static_cast<const char*>(buf), len));
        return int64_t(len);
    }
    int64_t seek(int64_t off, int whence) override {
        if (whence != kSEEK_SET || off < 0) return -kEINVAL;
        pos_ = size_t(off);
        snapshot_ = board_.get(key_);
        return off;
    }
    int fstat(StatInfo& st) override {
        st = StatInfo();
        st.mode = kS_IFREG | 0644;
        st.size = 4096;  // sysfs reports a page
        return 0;
    }

private:
    BoardState& board_;
    std::string key_;
    std::string snapshot_;
    size_t pos_ = 0;
};

}  // namespace

std::string BoardState::get(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);
    return values_[path];
}

void BoardState::set(const std::string& path, const std::string& value) {
    std::lock_guard<std::mutex> lock(mu_);
    values_[path] = value;
}

void BoardState::register_sysfs(Vfs& vfs, bool cartridge_inserted) {
    const std::string p = "/sys/devices/platform/";
    const std::map<std::string, std::string> defaults = {
        // Power: 1 = running on batteries, charge normal (3 = low, 4 = critical).
        {p + "lf1000-power/status", "1\n"},
        {p + "lf1000-power/shutdown", "0\n"},
        {p + "lf1000-power/voltage", "2800\n"},
        // Any value but "none" means a cartridge is present.
        {p + "lf1000-nand/cartridge", cartridge_inserted ? "nand\n" : "none\n"},
        {p + "lf1000-usbgadget/vbus", "0\n"},
        {p + "lf1000-usbgadget/gadget/gadget-lun0/enabled", "0\n"},
        {p + "lf1000-usbgadget/gadget/gadget-lun1/enabled", "0\n"},
        {p + "lf1000-usbgadget/gadget/gadget-lun0/needs_repair", "0\n"},
        {p + "lf1000-audio/spk_ctl", "0\n"},
    };
    for (const auto& [path, value] : defaults) {
        set(path, value);
        vfs.add_synthetic(path, [this, path = path](uint32_t) { return std::make_shared<SysfsFile>(*this, path); });
    }
}

}  // namespace didj::emu
