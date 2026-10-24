// OSS audio (/dev/dsp), which portaudio and Brio's audio module write PCM to.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

#include "emu/vfs.h"

namespace didj::emu {

// Where PCM goes. Implementations must be thread-safe (SDL calls back on
// its own thread).
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    virtual void configure(int rate, int channels) = 0;
    // Interleaved signed 16-bit samples.
    virtual void write(const int16_t* samples, size_t frames) = 0;
    // Frames written but not yet played.
    virtual size_t queued_frames() = 0;
};

// Discards audio but drains it at the configured rate, so the guest sees
// the same back-pressure as on hardware.
class NullAudioOutput : public AudioOutput {
public:
    void configure(int rate, int channels) override;
    void write(const int16_t* samples, size_t frames) override;
    size_t queued_frames() override;

private:
    std::mutex mu_;
    int rate_ = 8000;
    double queued_ = 0;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
    void drain();
};

void register_oss_device(Vfs& vfs, std::shared_ptr<AudioOutput> out);

}  // namespace didj::emu
