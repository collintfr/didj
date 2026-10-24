// The Didj's button input: an evdev device named "LF1000 Keyboard"
// (/dev/input/event0). Brio's button module maps these Linux key codes to
// its button bits (decoded from libButton.so):
//
//   KEY_UP/DOWN/RIGHT/LEFT -> Up/Down/Right/Left   KEY_A, KEY_B -> A, B
//   KEY_L, KEY_R -> shoulders   KEY_M -> Home (menu)   KEY_H -> Hint
//   KEY_P -> Pause   KEY_X -> Brightness
//   EV_SW 2 -> headphones, EV_SW 1 -> cartridge
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "emu/vfs.h"

namespace didj::emu {

enum class DidjButton { Up, Down, Left, Right, A, B, LeftShoulder, RightShoulder, Home, Hint, Pause, Brightness };

// Linux key codes the Didj's keyboard driver reports.
uint16_t linux_keycode(DidjButton b);

// "up", "a", "home", ... (lowercase); false if the name is unknown.
bool button_from_name(const std::string& name, DidjButton& out);

class InputHub {
public:
    // Thread-safe: the front end may call these from its own thread.
    void press(DidjButton b, bool down);
    void set_switch(uint16_t code, bool on);

    std::shared_ptr<OpenFile> open(uint32_t flags);
    void register_device(Vfs& vfs);  // char major 13, minor 64

    struct Event {
        int64_t time_us;
        uint16_t type, code;
        int32_t value;
    };

private:
    friend class EvdevFile;
    void post(uint16_t type, uint16_t code, int32_t value);

    std::mutex mu_;
    std::vector<std::weak_ptr<std::deque<Event>>> queues_;  // one per open file
    uint32_t switches_ = 0;                                   // bit per SW code
    uint8_t keys_[128] = {};                                  // key down state; Didj codes are all < 128
};

}  // namespace didj::emu
