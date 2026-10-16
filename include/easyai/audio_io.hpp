// easyai — microphone capture and speaker playback with the operating
// system's own audio API: ALSA on Linux (libasound), CoreAudio AudioQueue on
// macOS. PCM16 mono. Capture delivers fixed periods through a blocking
// read() paced by the device clock (no timer, no polling); playback is a
// blocking play of one clip, used sentence by sentence by easyai-assistant.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace easyai {
namespace audio {

// "alsa", "coreaudio" or "none" (built without an audio backend).
std::string backend_name();

class Capture {
public:
    Capture();
    ~Capture();
    Capture(const Capture &)             = delete;
    Capture & operator=(const Capture &) = delete;

    // device: "default" or a backend name (ALSA "plughw:1,0"). period_ms is
    // the size of one read(). Returns false with err set when the device
    // cannot be opened at rate_hz mono PCM16.
    bool open(const std::string & device, int rate_hz, int period_ms, std::string & err);
    void close();
    bool is_open() const;
    int  rate_hz() const;
    int  period_frames() const;

    // Blocks until one period is available; false on device failure.
    bool read(std::vector<std::int16_t> & period);
    // Drops whatever was captured while the caller was busy (its own beep or
    // voice through the speaker), so it is not heard as a question.
    void flush();
    const std::string & last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

class Playback {
public:
    Playback();
    ~Playback();
    Playback(const Playback &)             = delete;
    Playback & operator=(const Playback &) = delete;

    // Opens the device for one clip at rate_hz, plays all of it, drains and
    // closes. Blocking. device as in Capture::open.
    bool play(const std::string & device, const std::int16_t * pcm, std::size_t n, int rate_hz, std::string & err);
};

}  // namespace audio
}  // namespace easyai
