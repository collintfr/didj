// AF_UNIX stream sockets within one emulated process. The Didj's system
// daemons (monitord) talk to the app this way; here only the app's own
// endpoints exist, so a listener simply waits.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "emu/vfs.h"

namespace didj::emu {

struct SocketChannel {
    std::deque<uint8_t> to_server, to_client;
    bool server_open = true, client_open = true;
};

class UnixSocket : public OpenFile {
public:
    enum class State { Unbound, Listening, Connected };

    int64_t read(void* buf, size_t len) override;
    int64_t write(const void* buf, size_t len) override;
    int16_t poll_events() override;
    int fstat(StatInfo& st) override;
    ~UnixSocket() override;

    State state = State::Unbound;
    int type = 1;            // SOCK_STREAM
    std::string bound_path;  // for bind/getsockname
    std::shared_ptr<SocketChannel> channel;
    bool is_server_end = false;
    std::deque<std::shared_ptr<UnixSocket>> backlog;  // pending connections (listeners)
};

}  // namespace didj::emu
