// easyai — microphone capture and speaker playback with the operating
// system's own audio API: ALSA on Linux (libasound), CoreAudio AudioQueue on
// macOS. PCM16 mono. Capture delivers fixed periods through a blocking
// read() paced by the device clock (no timer, no polling); playback is a
// blocking play of one clip, used sentence by sentence by easyai::Assistant
// (the voice assistant inside easyai-server).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace easyai {
namespace audio {

// "alsa", "coreaudio" or "none" (built without an audio backend).
std::string backend_name();

// One microphone or speaker the operating system knows. Linux/ALSA: id is
// the ALSA name to put in [ASSISTANT] mic / speaker ("default",
// "plughw:1,0", "sysdefault:CARD=Generic_1" …), card the card name from
// /proc/asound/cards, name the human description. macOS/CoreAudio: id is
// "default" plus one entry per device ("coreaudio:<AudioDeviceID>"; only
// "default" is accepted by Capture/Playback today).
struct Device {
    std::string id;
    std::string name;
    std::string card;
    bool        capture    = false;
    bool        playback   = false;
    bool        is_default = false;
};

// Every device, "default" first. ALSA: snd_device_name_hint() for the
// names, /proc/asound/pcm for the capture/playback flags of the hw cards
// (one plughw:<card>,<dev> entry per pcm, so a box whose "default" is an
// HDMI-only card still shows the microphone). Built without a backend →
// empty list and err set. Nothing is opened: a listing never disturbs a
// running capture.
std::vector<Device> list_devices(std::string & err);

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
