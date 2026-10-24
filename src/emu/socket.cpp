#include "emu/socket.h"

#include <algorithm>

#include "emu/linux_abi.h"

namespace didj::emu {

using namespace lx;

UnixSocket::~UnixSocket() {
    if (channel) (is_server_end ? channel->server_open : channel->client_open) = false;
}

int64_t UnixSocket::read(void* buf, size_t len) {
    if (state != State::Connected) return -kENOTCONN;
    auto& in = is_server_end ? channel->to_server : channel->to_client;
    bool peer_open = is_server_end ? channel->client_open : channel->server_open;
    if (in.empty()) return peer_open ? (flags & kO_NONBLOCK ? -kEAGAIN : kWouldBlock) : 0;
    size_t n = std::min(len, in.size());
    std::copy(in.begin(), in.begin() + std::ptrdiff_t(n), static_cast<uint8_t*>(buf));
    in.erase(in.begin(), in.begin() + std::ptrdiff_t(n));
    return int64_t(n);
}

int64_t UnixSocket::write(const void* buf, size_t len) {
    if (state != State::Connected) return -kENOTCONN;
    bool peer_open = is_server_end ? channel->client_open : channel->server_open;
    if (!peer_open) return -kEPIPE;
    auto& out = is_server_end ? channel->to_client : channel->to_server;
    const auto* s = static_cast<const uint8_t*>(buf);
    out.insert(out.end(), s, s + len);
    return int64_t(len);
}

int16_t UnixSocket::poll_events() {
    switch (state) {
        case State::Listening: return backlog.empty() ? 0 : kPOLLIN;
        case State::Connected: {
            auto& in = is_server_end ? channel->to_server : channel->to_client;
            bool peer_open = is_server_end ? channel->client_open : channel->server_open;
            return int16_t((in.empty() ? 0 : kPOLLIN) | kPOLLOUT | (peer_open ? 0 : kPOLLHUP));
        }
        default: return kPOLLOUT;
    }
}

int UnixSocket::fstat(StatInfo& st) {
    st = StatInfo();
    st.mode = kS_IFSOCK | 0777;
    return 0;
}

}  // namespace didj::emu
