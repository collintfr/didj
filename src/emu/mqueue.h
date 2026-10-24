// POSIX message queues (mq_open & co.), which Brio uses for all event
// passing between its modules and threads.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "emu/vfs.h"

namespace didj::emu {

struct MessageQueue {
    uint32_t maxmsg = 10, msgsize = 8192;
    // Higher priority first; FIFO within a priority.
    std::map<uint32_t, std::deque<std::vector<uint8_t>>, std::greater<uint32_t>> messages;
    size_t count = 0;
    // mq_notify registration: signal to raise when a message arrives in an
    // empty queue (SIGEV_SIGNAL only).
    int notify_signo = 0;
    uint32_t notify_value = 0;
    bool notify_armed = false;

    bool full() const { return count >= maxmsg; }
    bool empty() const { return count == 0; }
    void push(uint32_t prio, std::vector<uint8_t> msg) {
        messages[prio].push_back(std::move(msg));
        ++count;
    }
    std::vector<uint8_t> pop(uint32_t& prio) {
        auto it = messages.begin();
        prio = it->first;
        std::vector<uint8_t> m = std::move(it->second.front());
        it->second.pop_front();
        if (it->second.empty()) messages.erase(it);
        --count;
        return m;
    }
};

// A descriptor for an open queue.
class MqueueFile : public OpenFile {
public:
    explicit MqueueFile(std::shared_ptr<MessageQueue> q) : q(std::move(q)) {}
    int16_t poll_events() override { return int16_t((q->empty() ? 0 : 0x1) | (q->full() ? 0 : 0x4)); }
    int fstat(StatInfo& st) override {
        st = StatInfo();
        st.mode = 0100600;
        return 0;
    }
    std::shared_ptr<MessageQueue> q;
};

}  // namespace didj::emu
